#pragma once

// Cập nhật qua GitHub Releases, có kiểm tra chữ ký số.
//
// Mỗi bản phát hành (tạo bởi GitHub Actions, xem tools\sign-release.ps1) có:
//   manifest.txt   amlich-update-manifest=1 / version=X.Y.Z / tag=vX.Y.Z /
//                  file=<tên>;<kích thước>;<sha256> / notes=<ghi chú>
//   manifest.sig   chữ ký ECDSA P-256 + SHA-256 (64 byte r||s) của manifest.txt
//   AmLichVN.exe, AmLichTap.dll
//
// Ứng dụng chỉ cài khi: chữ ký hợp lệ với khóa công khai biên dịch sẵn
// (update_pubkey.h), phiên bản mới hơn bản đang chạy, và kích thước + SHA-256
// của từng file khớp manifest. Bước cài (chạy với quyền Administrator) đọc toàn
// bộ file vào bộ nhớ rồi tự kiểm tra lại từ đầu, không tin kết quả kiểm tra của
// tiến trình thường.

#include <windows.h>

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/shared.h"
#include "../common/update_pubkey.h"

namespace upd {

constexpr wchar_t kDefaultRepo[] =
    L"longho-lhu/vietnamese-lunar-calendar-for-windows";
constexpr const char* kUpdateFiles[] = {"AmLichVN.exe", "AmLichTap.dll"};
constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kMaxSignatureBytes = 1024;
constexpr size_t kMaxBinaryBytes = 32 * 1024 * 1024;

// Mã thoát của "AmLichVN.exe --apply-update <thư mục>".
enum ApplyExitCode : DWORD {
    kApplyOk = 0,
    kApplyBadArgs = 1,
    kApplyBadSignature = 2,
    kApplyNotNewer = 3,
    kApplyIoError = 4,
};

////////////////////////////////////////////////////////////////////////////////
// Chuỗi
////////////////////////////////////////////////////////////////////////////////

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return L"";
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return "";
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

inline std::wstring Win32ErrorText(DWORD err) {
    WCHAR* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE module = nullptr;
    if (err >= 12000 && err <= 12200) {  // lỗi WinHTTP
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
        module = GetModuleHandleW(L"winhttp.dll");
    }
    DWORD n = FormatMessageW(flags, module, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = n ? std::wstring(buf, n) : L"";
    if (buf) {
        LocalFree(buf);
    }
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' ||
                          s.back() == L' ' || s.back() == L'.')) {
        s.pop_back();
    }
    WCHAR code[32];
    swprintf_s(code, L"mã %lu", err);
    return s.empty() ? code : s + L" (" + code + L")";
}

// Chấp nhận "chủ/tên", "https://github.com/chủ/tên", "git@github.com:chủ/tên.git".
// Trả về chuỗi rỗng nếu không hợp lệ.
inline std::wstring NormalizeRepo(std::wstring s) {
    auto trim = [](std::wstring& x) {
        while (!x.empty() && iswspace(x.front())) {
            x.erase(x.begin());
        }
        while (!x.empty() && iswspace(x.back())) {
            x.pop_back();
        }
    };
    trim(s);
    for (PCWSTR prefix :
         {L"https://github.com/", L"http://github.com/", L"github.com/",
          L"git@github.com:", L"ssh://git@github.com/"}) {
        size_t n = wcslen(prefix);
        if (s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0) {
            s = s.substr(n);
            break;
        }
    }
    while (!s.empty() && s.back() == L'/') {
        s.pop_back();
    }
    if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".git") == 0) {
        s.resize(s.size() - 4);
    }
    size_t slash = s.find(L'/');
    if (slash == std::wstring::npos || slash == 0 || slash + 1 == s.size() ||
        s.find(L'/', slash + 1) != std::wstring::npos || s.size() > 140) {
        return L"";
    }
    for (wchar_t c : s) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' ||
                  c == L'.' || c == L'/';
        if (!ok) {
            return L"";
        }
    }
    if (s.substr(0, slash) == L"." || s.substr(0, slash) == L".." ||
        s.substr(slash + 1) == L"." || s.substr(slash + 1) == L"..") {
        return L"";
    }
    return s;
}

////////////////////////////////////////////////////////////////////////////////
// Phiên bản
////////////////////////////////////////////////////////////////////////////////

struct Version {
    long part[3] = {0, 0, 0};
};

inline bool ParseVersion(const std::string& s, Version* v) {
    int idx = 0;
    long cur = -1;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 99999) {
                return false;
            }
        } else if (c == '.') {
            if (cur < 0 || idx >= 2) {
                return false;
            }
            v->part[idx++] = cur;
            cur = -1;
        } else {
            return false;
        }
    }
    if (cur < 0 || idx != 2) {
        return false;
    }
    v->part[2] = cur;
    return true;
}

inline int CompareVersions(const Version& a, const Version& b) {
    for (int i = 0; i < 3; i++) {
        if (a.part[i] != b.part[i]) {
            return a.part[i] < b.part[i] ? -1 : 1;
        }
    }
    return 0;
}

inline Version CurrentVersion() {
    Version v;
    ParseVersion(WideToUtf8(AMLICH_VERSION), &v);
    return v;
}

////////////////////////////////////////////////////////////////////////////////
// Manifest
////////////////////////////////////////////////////////////////////////////////

struct ManifestFile {
    std::string name;
    uint64_t size = 0;
    std::string sha256;  // hex chữ thường
};

struct Manifest {
    std::string version;
    std::string tag;
    std::wstring notes;
    std::vector<ManifestFile> files;
};

inline bool IsSafeTag(const std::string& tag) {
    if (tag.empty() || tag.size() > 64) {
        return false;
    }
    for (char c : tag) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return false;
        }
    }
    return tag != "." && tag != "..";
}

inline bool ParseManifest(const std::string& text,
                          Manifest* m,
                          std::wstring* err) {
    *m = Manifest{};
    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first) {
            if (line != "amlich-update-manifest=1") {
                *err = L"manifest không đúng định dạng";
                return false;
            }
            first = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") {
            m->version = v;
        } else if (k == "tag") {
            m->tag = v;
        } else if (k == "notes") {
            std::string out;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '\\' && i + 1 < v.size()) {
                    char n = v[++i];
                    out += n == 'n' ? '\n' : n;
                } else {
                    out += v[i];
                }
            }
            m->notes = Utf8ToWide(out);
        } else if (k == "file") {
            size_t a = v.find(';'), b = v.find(';', a + 1);
            if (a == std::string::npos || b == std::string::npos) {
                *err = L"dòng file trong manifest sai định dạng";
                return false;
            }
            ManifestFile f;
            f.name = v.substr(0, a);
            std::string size = v.substr(a + 1, b - a - 1);
            f.sha256 = v.substr(b + 1);
            if (size.empty() || size.size() > 12 ||
                size.find_first_not_of("0123456789") != std::string::npos) {
                *err = L"kích thước file trong manifest không hợp lệ";
                return false;
            }
            f.size = std::stoull(size);
            if (f.sha256.size() != 64 ||
                f.sha256.find_first_not_of("0123456789abcdef") !=
                    std::string::npos) {
                *err = L"mã SHA-256 trong manifest không hợp lệ";
                return false;
            }
            bool allowed = false;
            for (const char* name : kUpdateFiles) {
                allowed |= f.name == name;
            }
            for (const auto& other : m->files) {
                allowed &= other.name != f.name;
            }
            if (!allowed || f.size == 0 || f.size > kMaxBinaryBytes) {
                *err = L"manifest chứa file không được phép";
                return false;
            }
            m->files.push_back(f);
        }
    }
    Version v;
    if (first || !ParseVersion(m->version, &v) || !IsSafeTag(m->tag) ||
        m->files.size() != ARRAYSIZE(kUpdateFiles)) {
        *err = L"manifest thiếu thông tin phiên bản hoặc file";
        return false;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Mật mã (Windows CNG)
////////////////////////////////////////////////////////////////////////////////

inline bool Sha256(const void* data, size_t len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        return false;
    }
    NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)data, (ULONG)len, out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return BCRYPT_SUCCESS(st);
}

inline std::string Sha256Hex(const std::string& data) {
    unsigned char h[32];
    if (!Sha256(data.data(), data.size(), h)) {
        return "";
    }
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (unsigned char c : h) {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s;
}

inline bool VerifySignature(const std::string& data, const std::string& sig) {
    if (sig.size() != 64) {
        return false;
    }
    unsigned char hash[32];
    if (!Sha256(data.data(), data.size(), hash)) {
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(
            alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
            (PUCHAR)kUpdatePublicKey, sizeof(kUpdatePublicKey), 0))) {
        ok = BCryptVerifySignature(key, nullptr, hash, sizeof(hash),
                                   (PUCHAR)sig.data(), (ULONG)sig.size(),
                                   0) == 0;
    }
    if (key) {
        BCryptDestroyKey(key);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

////////////////////////////////////////////////////////////////////////////////
// HTTPS (WinHTTP)
////////////////////////////////////////////////////////////////////////////////

struct InternetHandle {
    HINTERNET h = nullptr;
    ~InternetHandle() {
        if (h) {
            WinHttpCloseHandle(h);
        }
    }
};

inline bool HttpGet(const std::wstring& url,
                    size_t maxBytes,
                    std::string* out,
                    std::wstring* err,
                    const std::atomic<bool>* cancel = nullptr) {
    out->clear();
    WCHAR host[256], path[2048];
    URL_COMPONENTS uc{sizeof(uc)};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) ||
        uc.nScheme != INTERNET_SCHEME_HTTPS) {
        *err = L"địa chỉ không hợp lệ: " + url;
        return false;
    }

    auto fail = [&](PCWSTR what) {
        *err = std::wstring(what) + L": " + Win32ErrorText(GetLastError());
        return false;
    };

    InternetHandle session;
    session.h = WinHttpOpen(L"AmLichVN-Updater/" AMLICH_VERSION,
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        session.h = WinHttpOpen(L"AmLichVN-Updater/" AMLICH_VERSION,
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                0);
    }
    if (!session.h) {
        return fail(L"Không khởi tạo được kết nối mạng");
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 20000, 30000);

    InternetHandle connect;
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h) {
        return fail(L"Không kết nối được tới máy chủ");
    }
    InternetHandle request;
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr,
                                   WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!request.h) {
        return fail(L"Không tạo được yêu cầu");
    }
    // Không gửi cookie hay thông tin đăng nhập nào.
    DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &opt,
                     sizeof(opt));

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return fail(L"Không tải được dữ liệu");
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WCHAR buf[64];
        swprintf_s(buf, L"máy chủ trả về mã HTTP %lu", status);
        *err = buf;
        if (status == 404) {
            *err += L" (không tìm thấy)";
        }
        return false;
    }

    for (;;) {
        if (cancel && cancel->load()) {
            *err = L"đã hủy";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            return fail(L"Lỗi khi tải dữ liệu");
        }
        if (avail == 0) {
            break;
        }
        if (out->size() + avail > maxBytes) {
            *err = L"dữ liệu tải về lớn bất thường";
            return false;
        }
        size_t old = out->size();
        out->resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out->data() + old, avail, &read)) {
            return fail(L"Lỗi khi tải dữ liệu");
        }
        out->resize(old + read);
    }
    return true;
}

inline std::wstring ReleaseDownloadUrl(const std::wstring& repo,
                                       const std::string& tag,
                                       const std::string& file) {
    return L"https://github.com/" + repo + L"/releases/download/" +
           Utf8ToWide(tag) + L"/" + Utf8ToWide(file);
}

inline std::wstring ReleasePageUrl(const std::wstring& repo,
                                   const std::string& tag = "") {
    return L"https://github.com/" + repo + L"/releases" +
           (tag.empty() ? L"" : L"/tag/" + Utf8ToWide(tag));
}

////////////////////////////////////////////////////////////////////////////////
// Kiểm tra / tải bản cập nhật (tiến trình thường)
////////////////////////////////////////////////////////////////////////////////

struct CheckResult {
    enum Kind { Error, UpToDate, Available } kind = Error;
    std::wstring repo;
    std::wstring error;
    Manifest manifest;
    std::string manifestRaw;
    std::string signatureRaw;
};

inline CheckResult CheckForUpdate(const std::wstring& repoInput) {
    CheckResult r;
    r.repo = NormalizeRepo(repoInput);
    if (r.repo.empty()) {
        r.error = L"Tên kho GitHub không hợp lệ: \"" + repoInput + L"\"";
        return r;
    }

    std::wstring err;
    if (!HttpGet(L"https://github.com/" + r.repo +
                     L"/releases/latest/download/manifest.txt",
                 kMaxManifestBytes, &r.manifestRaw, &err)) {
        r.error = L"Không tải được manifest của bản phát hành mới nhất: " + err;
        if (err.find(L"404") != std::wstring::npos) {
            r.error += L".\nKho chưa có bản phát hành (Release) nào có manifest, "
                       L"hoặc tên kho sai.";
        }
        return r;
    }
    Manifest untrusted;
    if (!ParseManifest(r.manifestRaw, &untrusted, &err)) {
        r.error = L"Manifest không hợp lệ: " + err;
        return r;
    }
    if (!HttpGet(ReleaseDownloadUrl(r.repo, untrusted.tag, "manifest.sig"),
                 kMaxSignatureBytes, &r.signatureRaw, &err)) {
        r.error = L"Không tải được chữ ký số (manifest.sig): " + err;
        return r;
    }
    if (!VerifySignature(r.manifestRaw, r.signatureRaw)) {
        r.error = L"CHỮ KÝ SỐ KHÔNG HỢP LỆ - bản phát hành này không do nhà "
                  L"phát triển ký. Không cập nhật.";
        return r;
    }
    ParseManifest(r.manifestRaw, &r.manifest, &err);  // đã kiểm tra ở trên

    Version latest;
    ParseVersion(r.manifest.version, &latest);
    r.kind = CompareVersions(latest, CurrentVersion()) > 0 ? CheckResult::Available
                                                          : CheckResult::UpToDate;
    return r;
}

inline bool WriteWholeFile(const std::wstring& path,
                           const std::string& data,
                           std::wstring* err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = L"không ghi được " + path + L": " + Win32ErrorText(GetLastError());
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) &&
              written == data.size() && FlushFileBuffers(f);
    DWORD e = GetLastError();
    CloseHandle(f);
    if (!ok) {
        *err = L"không ghi được " + path + L": " + Win32ErrorText(e);
    }
    return ok;
}

inline bool ReadWholeFile(const std::wstring& path,
                          size_t maxBytes,
                          std::string* out,
                          std::wstring* err) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = L"không đọc được " + path + L": " + Win32ErrorText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= (LONGLONG)maxBytes;
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = out->empty() ||
             (ReadFile(f, out->data(), (DWORD)out->size(), &read, nullptr) &&
              read == out->size());
    }
    CloseHandle(f);
    if (!ok) {
        *err = L"không đọc được " + path;
    }
    return ok;
}

inline std::wstring LocalUpdateRoot() {
    WCHAR buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
    std::wstring base = (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n)
                                                  : std::wstring(L"C:\\Temp");
    return base + L"\\AmLichVN\\update";
}

// Xóa cả cây thư mục (dùng cho thư mục tải tạm).
inline void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    DeleteTree(p);
                } else {
                    RemoveDirectoryW(p.c_str());
                }
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Tải các file của bản cập nhật vào thư mục tạm, kiểm tra SHA-256.
inline bool DownloadUpdate(const CheckResult& c,
                           std::wstring* stagingDir,
                           std::wstring* err,
                           const std::atomic<bool>* cancel) {
    std::wstring root = LocalUpdateRoot();
    DeleteTree(root);
    *stagingDir = root + L"\\" + Utf8ToWide(c.manifest.tag);
    int rc = SHCreateDirectoryExW(nullptr, stagingDir->c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
        *err = L"không tạo được thư mục tạm " + *stagingDir;
        return false;
    }
    for (const auto& f : c.manifest.files) {
        std::string data;
        std::wstring e;
        if (!HttpGet(ReleaseDownloadUrl(c.repo, c.manifest.tag, f.name),
                     kMaxBinaryBytes, &data, &e, cancel)) {
            *err = L"Không tải được " + Utf8ToWide(f.name) + L": " + e;
            return false;
        }
        if (data.size() != f.size || Sha256Hex(data) != f.sha256) {
            *err = L"File " + Utf8ToWide(f.name) +
                   L" tải về không khớp mã SHA-256 trong manifest đã ký.";
            return false;
        }
        if (!WriteWholeFile(*stagingDir + L"\\" + Utf8ToWide(f.name), data, err)) {
            return false;
        }
    }
    return WriteWholeFile(*stagingDir + L"\\manifest.txt", c.manifestRaw, err) &&
           WriteWholeFile(*stagingDir + L"\\manifest.sig", c.signatureRaw, err);
}

////////////////////////////////////////////////////////////////////////////////
// Cài bản cập nhật (chạy với quyền Administrator)
////////////////////////////////////////////////////////////////////////////////

// Xóa các file *.old-* còn sót từ lần cập nhật trước; file đang bị khóa thì hẹn
// xóa khi khởi động lại máy.
inline void CleanupOldFiles(const std::wstring& installDir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((installDir + L"\\*.old-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring p = installDir + L"\\" + fd.cFileName;
        if (!DeleteFileW(p.c_str())) {
            MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

inline DWORD ApplyUpdate(const std::wstring& stagingDir,
                         const std::wstring& installDir,
                         std::wstring* log) {
    std::string manifestRaw, sig;
    std::wstring err;
    if (!ReadWholeFile(stagingDir + L"\\manifest.txt", kMaxManifestBytes,
                       &manifestRaw, &err) ||
        !ReadWholeFile(stagingDir + L"\\manifest.sig", kMaxSignatureBytes, &sig,
                       &err)) {
        *log = err;
        return kApplyBadArgs;
    }
    Manifest m;
    if (!VerifySignature(manifestRaw, sig)) {
        *log = L"chữ ký số không hợp lệ";
        return kApplyBadSignature;
    }
    if (!ParseManifest(manifestRaw, &m, &err)) {
        *log = err;
        return kApplyBadSignature;
    }
    Version v;
    ParseVersion(m.version, &v);
    if (CompareVersions(v, CurrentVersion()) <= 0) {
        *log = L"phiên bản " + Utf8ToWide(m.version) +
               L" không mới hơn bản đang cài " AMLICH_VERSION;
        return kApplyNotNewer;
    }

    // Đọc + kiểm tra toàn bộ file trong bộ nhớ trước khi đụng tới bản đang cài.
    std::vector<std::pair<std::wstring, std::string>> payload;
    for (const auto& f : m.files) {
        std::string data;
        std::wstring name = Utf8ToWide(f.name);
        if (!ReadWholeFile(stagingDir + L"\\" + name, kMaxBinaryBytes, &data,
                           &err)) {
            *log = err;
            return kApplyBadArgs;
        }
        if (data.size() != f.size || Sha256Hex(data) != f.sha256) {
            *log = name + L" không khớp SHA-256 trong manifest";
            return kApplyBadSignature;
        }
        payload.emplace_back(name, std::move(data));
    }

    CleanupOldFiles(installDir);

    // Bước 1: ghi file mới ra *.new.
    for (const auto& [name, data] : payload) {
        if (!WriteWholeFile(installDir + L"\\" + name + L".new", data, &err)) {
            *log = err;
            for (const auto& p : payload) {
                DeleteFileW((installDir + L"\\" + p.first + L".new").c_str());
            }
            return kApplyIoError;
        }
    }

    // Bước 2: đổi tên file đang dùng (được phép kể cả khi exe đang chạy / DLL
    // đang được nạp), rồi đưa file mới vào chỗ. Lỗi thì hoàn tác.
    WCHAR suffix[32];
    swprintf_s(suffix, L".old-%llu", GetTickCount64());
    std::vector<std::wstring> swapped;
    bool ok = true;
    for (const auto& [name, data] : payload) {
        std::wstring cur = installDir + L"\\" + name;
        std::wstring old = cur + suffix;
        std::wstring neu = cur + L".new";
        bool hadOld = GetFileAttributesW(cur.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (hadOld && !MoveFileExW(cur.c_str(), old.c_str(), 0)) {
            err = L"không đổi tên được " + cur + L": " +
                  Win32ErrorText(GetLastError());
            ok = false;
            break;
        }
        if (!MoveFileExW(neu.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            err = L"không thay được " + cur + L": " +
                  Win32ErrorText(GetLastError());
            if (hadOld) {
                MoveFileExW(old.c_str(), cur.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            ok = false;
            break;
        }
        swapped.push_back(name);
    }
    if (!ok) {
        for (const auto& name : swapped) {
            std::wstring cur = installDir + L"\\" + name;
            MoveFileExW((cur + suffix).c_str(), cur.c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        }
        for (const auto& p : payload) {
            DeleteFileW((installDir + L"\\" + p.first + L".new").c_str());
        }
        *log = err;
        return kApplyIoError;
    }

    // File cũ còn bị khóa: hẹn xóa khi khởi động lại máy (lần cập nhật sau
    // cũng dọn).
    for (const auto& name : swapped) {
        std::wstring old = installDir + L"\\" + name + suffix;
        if (!DeleteFileW(old.c_str())) {
            MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    *log = L"đã cài phiên bản " + Utf8ToWide(m.version);
    return kApplyOk;
}

}  // namespace upd
