#pragma once

// Bảo vệ thư mục dữ liệu dùng chung %ProgramData%\AmLichVN (bản cài thường).
//
// Thư mục này được cả AmLichVN.exe của mọi người dùng lẫn AmLichTap.dll (chạy
// trong tiến trình bảng lịch, có thể là AppContainer) dùng chung, và được các
// tiến trình quyền Administrator (bộ cài, gỡ cài, cập nhật) đụng tới. Vì vậy:
//
//  - Thư mục gốc do Administrators sở hữu, DACL được bảo vệ (không kế thừa):
//    người dùng thường chỉ ĐỌC được, không tạo / xóa / đổi tên mục con, nên
//    không ai đặt được junction / symlink / hard link vào đó.
//  - settings.ini: người dùng thường đọc + ghi nội dung (không xóa, không đổi
//    quyền); AppContainer chỉ đọc.
//  - status\: người dùng thường (và AppContainer) chỉ được TẠO FILE; file tạo
//    ra thuộc về người tạo (CREATOR OWNER), người khác không sửa được. Tên file
//    mang SID người dùng (xem shared.h) và bên đọc kiểm tra chủ sở hữu.
//
// Các hàm Secure / Remove chạy với quyền Administrator. Mọi thao tác đều mở
// tương đối từ handle thư mục cha (NtCreateFile + RootDirectory) với
// FILE_OPEN_REPARSE_POINT, nên không bao giờ đi theo reparse point; ACL được
// đặt trực tiếp lên handle (SetKernelObjectSecurity), không lan truyền theo
// đường dẫn.

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>

#include <string>
#include <vector>

namespace datadir {

// --- ACL --------------------------------------------------------------------
// 0x1200a9 = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE
// 0x1200ab = liệt kê + TẠO FILE + duyệt + đọc thuộc tính (không tạo thư mục
//            con, không xóa)
// 0x12019f = FILE_GENERIC_READ | FILE_GENERIC_WRITE (không DELETE, WRITE_DAC)
// 0x120089 = FILE_GENERIC_READ
constexpr wchar_t kRootSddl[] =
    L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
    L"(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)";
constexpr wchar_t kStatusSddl[] =
    L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
    L"(A;;0x1200ab;;;BU)(A;;0x1200ab;;;AC)"
    L"(A;OIIO;FA;;;CO)(A;OIIO;0x12019f;;;AC)";
constexpr wchar_t kIniSddl[] =
    L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x12019f;;;BU)(A;;0x120089;;;AC)";
// Khóa tạm một mục trước khi xóa: chỉ SYSTEM + Administrators.
constexpr wchar_t kAdminOnlySddl[] =
    L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

constexpr wchar_t kDirName[] = L"AmLichVN";
constexpr wchar_t kIniName[] = L"settings.ini";
constexpr wchar_t kStatusName[] = L"status";

struct Sd {
    PSECURITY_DESCRIPTOR p = nullptr;
    explicit Sd(PCWSTR sddl) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &p, nullptr);
    }
    ~Sd() {
        if (p) {
            LocalFree(p);
        }
    }
    Sd(const Sd&) = delete;
    Sd& operator=(const Sd&) = delete;
};

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() { reset(); }
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    void reset() {
        if (h) {
            CloseHandle(h);
            h = nullptr;
        }
    }
    explicit operator bool() const { return h != nullptr; }
};

// Thư mục dữ liệu của bản cài thường. Lấy từ Known Folder thay vì biến môi
// trường (tiến trình Admin không nên tin biến môi trường của người dùng).
inline std::wstring ProgramDataDir() {
    PWSTR p = nullptr;
    std::wstring result = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)) && p) {
        result = p;
    }
    CoTaskMemFree(p);
    return result;
}

inline std::wstring DataDirPath() {
    return ProgramDataDir() + L"\\" + kDirName;
}

// --- NtCreateFile tương đối -------------------------------------------------

using PFN_NtCreateFile = NTSTATUS(NTAPI*)(PHANDLE,
                                          ACCESS_MASK,
                                          POBJECT_ATTRIBUTES,
                                          PIO_STATUS_BLOCK,
                                          PLARGE_INTEGER,
                                          ULONG,
                                          ULONG,
                                          ULONG,
                                          ULONG,
                                          PVOID,
                                          ULONG);

constexpr NTSTATUS kStatusNotFound = (NTSTATUS)0xC0000034L;      // OBJECT_NAME_NOT_FOUND
constexpr NTSTATUS kStatusPathNotFound = (NTSTATUS)0xC000003AL;  // OBJECT_PATH_NOT_FOUND
constexpr NTSTATUS kStatusNotADirectory = (NTSTATUS)0xC0000103L;
constexpr NTSTATUS kStatusFileIsADirectory = (NTSTATUS)0xC00000BAL;

#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif

// Mở / tạo `name` (một thành phần, không có '\') bên trong thư mục `parent`.
// Không bao giờ đi theo reparse point.
inline Handle OpenRel(HANDLE parent,
                      const std::wstring& name,
                      ACCESS_MASK access,
                      ULONG disposition,
                      ULONG options,
                      PSECURITY_DESCRIPTOR sd,
                      NTSTATUS* statusOut) {
    static auto ntCreateFile = reinterpret_cast<PFN_NtCreateFile>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    NTSTATUS st = (NTSTATUS)0xC0000001L;
    HANDLE h = nullptr;
    if (ntCreateFile && !name.empty() && name.find(L'\\') == std::wstring::npos &&
        name != L"." && name != L"..") {
        UNICODE_STRING us;
        us.Buffer = const_cast<PWSTR>(name.c_str());
        us.Length = (USHORT)(name.size() * sizeof(WCHAR));
        us.MaximumLength = us.Length;
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, parent, sd);
        IO_STATUS_BLOCK iosb{};
        st = ntCreateFile(&h, access | SYNCHRONIZE, &oa, &iosb, nullptr,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          disposition,
                          options | FILE_OPEN_REPARSE_POINT |
                              FILE_OPEN_FOR_BACKUP_INTENT |
                              FILE_SYNCHRONOUS_IO_NONALERT,
                          nullptr, 0);
        if (st < 0) {
            h = nullptr;
        }
    }
    if (statusOut) {
        *statusOut = st;
    }
    return Handle(h);
}

inline bool GetAttrs(HANDLE h, DWORD* attrs) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
        return false;
    }
    *attrs = tag.FileAttributes;
    return true;
}

inline bool SetSd(HANDLE h, PCWSTR sddl) {
    Sd sd(sddl);
    return sd.p && SetKernelObjectSecurity(
                       h,
                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                           PROTECTED_DACL_SECURITY_INFORMATION,
                       sd.p);
}

inline bool DeleteByHandle(HANDLE h) {
#if defined(FILE_DISPOSITION_FLAG_POSIX_SEMANTICS)
    FILE_DISPOSITION_INFO_EX ex{FILE_DISPOSITION_FLAG_DELETE |
                                FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(h, FileDispositionInfoEx, &ex, sizeof(ex))) {
        return true;
    }
#endif
    FILE_DISPOSITION_INFO d{TRUE};
    return SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof(d)) != FALSE;
}

// Tên các mục con của thư mục (đọc qua handle, không qua đường dẫn).
inline std::vector<std::wstring> ListChildren(HANDLE dir) {
    std::vector<std::wstring> names;
    alignas(8) BYTE buf[16 * 1024];
    FILE_INFO_BY_HANDLE_CLASS cls = FileFullDirectoryRestartInfo;
    while (GetFileInformationByHandleEx(dir, cls, buf, sizeof(buf))) {
        cls = FileFullDirectoryInfo;
        auto* e = reinterpret_cast<FILE_FULL_DIR_INFO*>(buf);
        for (;;) {
            std::wstring n(e->FileName, e->FileNameLength / sizeof(WCHAR));
            if (n != L"." && n != L"..") {
                names.push_back(n);
            }
            if (!e->NextEntryOffset) {
                break;
            }
            e = reinterpret_cast<FILE_FULL_DIR_INFO*>(
                reinterpret_cast<BYTE*>(e) + e->NextEntryOffset);
        }
    }
    return names;
}

// Xóa `name` trong `parent` (cả cây nếu là thư mục thật). Reparse point bị xóa
// chính nó, không đụng tới đích. Thư mục được khóa (chỉ Admin) trước khi duyệt
// để không ai thêm mục con trong lúc xóa.
inline bool DeleteTreeRel(HANDLE parent, const std::wstring& name, int depth = 0) {
    NTSTATUS st;
    Handle h = OpenRel(parent, name,
                       DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER |
                           FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                       FILE_OPEN, 0, nullptr, &st);
    if (!h) {
        return st == kStatusNotFound || st == kStatusPathNotFound;
    }
    DWORD attrs = 0;
    if (!GetAttrs(h.h, &attrs)) {
        return false;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (depth > 32) {
            return false;
        }
        SetSd(h.h, kAdminOnlySddl);
        for (const auto& child : ListChildren(h.h)) {
            DeleteTreeRel(h.h, child, depth + 1);
        }
    }
    return DeleteByHandle(h.h);
}

// Mở (tạo nếu chưa có) thư mục con `name` là thư mục THẬT. Nếu chỗ đó đang là
// file / reparse point thì xóa đi rồi tạo lại.
inline Handle OpenOrCreateDir(HANDLE parent, const std::wstring& name, PCWSTR sddl) {
    Sd sd(sddl);
    for (int attempt = 0; attempt < 3; attempt++) {
        NTSTATUS st;
        Handle h = OpenRel(parent, name,
                           READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE |
                               FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                               FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
                           FILE_OPEN_IF, FILE_DIRECTORY_FILE, sd.p, &st);
        if (!h) {
            if (st == kStatusNotADirectory) {
                DeleteTreeRel(parent, name);
                continue;
            }
            return Handle();
        }
        DWORD attrs = 0;
        if (!GetAttrs(h.h, &attrs)) {
            return Handle();
        }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            DeleteByHandle(h.h);
            continue;
        }
        if (!SetSd(h.h, sddl)) {
            return Handle();
        }
        return h;
    }
    return Handle();
}

inline bool EnablePrivileges() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    bool all = true;
    for (PCWSTR name :
         {SE_BACKUP_NAME, SE_RESTORE_NAME, SE_TAKE_OWNERSHIP_NAME}) {
        TOKEN_PRIVILEGES tp{1};
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) ||
            !AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr,
                                   nullptr) ||
            GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
            all = false;
        }
    }
    CloseHandle(token);
    return all;
}

inline Handle OpenProgramData() {
    return Handle(CreateFileW(ProgramDataDir().c_str(),
                              FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                                  FILE_ADD_SUBDIRECTORY | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr));
}

// Mã thoát của --secure-datadir / --remove-datadir.
enum : DWORD {
    kOk = 0,
    kErrNotAdmin = 20,
    kErrRoot = 21,
    kErrStatus = 22,
    kErrIni = 23,
    kErrRemove = 24,
};

// settings.ini mặc định: settings.default.ini (UTF-8, cạnh AmLichVN.exe) đổi
// sang UTF-16 để GetPrivateProfileString và Notepad đọc đúng tiếng Việt.
inline std::string DefaultIniBytes(const std::wstring& defaultIniPath) {
    std::string utf8;
    HANDLE f = CreateFileW(defaultIniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart < 256 * 1024) {
            utf8.resize((size_t)size.QuadPart);
            DWORD read = 0;
            if (!utf8.empty() &&
                !ReadFile(f, utf8.data(), (DWORD)utf8.size(), &read, nullptr)) {
                read = 0;
            }
            utf8.resize(read);
        }
        CloseHandle(f);
    }
    if (utf8.size() >= 3 && (unsigned char)utf8[0] == 0xEF &&
        (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF) {
        utf8.erase(0, 3);
    }
    if (utf8.empty()) {
        utf8 = "[AmLich]\r\n";
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    w.insert(w.begin(), L'\xFEFF');
    return std::string(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(WCHAR));
}

// --secure-datadir: tạo / sửa %ProgramData%\AmLichVN cho đúng ACL ở trên.
// Dọn mọi thứ không thuộc về chương trình (log cũ, file lạ, reparse point) và
// toàn bộ nội dung status\ (chỉ là trạng thái tạm, DLL sẽ ghi lại).
inline DWORD Secure(const std::wstring& defaultIniPath) {
    if (!EnablePrivileges()) {
        return kErrNotAdmin;
    }
    Handle pd = OpenProgramData();
    if (!pd) {
        return kErrRoot;
    }
    Handle root = OpenOrCreateDir(pd.h, kDirName, kRootSddl);
    if (!root) {
        return kErrRoot;
    }
    // Từ đây người dùng thường không thêm / đổi được mục con nào của root.
    for (const auto& name : ListChildren(root.h)) {
        if (_wcsicmp(name.c_str(), kIniName) != 0 &&
            _wcsicmp(name.c_str(), kStatusName) != 0) {
            DeleteTreeRel(root.h, name);
        }
    }

    DWORD rc = kOk;
    if (Handle status = OpenOrCreateDir(root.h, kStatusName, kStatusSddl)) {
        for (const auto& name : ListChildren(status.h)) {
            DeleteTreeRel(status.h, name);
        }
    } else {
        rc = kErrStatus;
    }

    // settings.ini: giữ nội dung nếu là file thường chỉ có một tên; ngược lại
    // xóa và tạo lại từ bản mặc định.
    NTSTATUS st;
    Handle ini = OpenRel(root.h, kIniName,
                         READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE |
                             FILE_READ_ATTRIBUTES,
                         FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr, &st);
    if (!ini && st == kStatusFileIsADirectory) {
        DeleteTreeRel(root.h, kIniName);
    }
    if (ini) {
        DWORD attrs = 0;
        FILE_STANDARD_INFO si{};
        bool regular = GetAttrs(ini.h, &attrs) &&
                       !(attrs & (FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DIRECTORY)) &&
                       GetFileInformationByHandleEx(ini.h, FileStandardInfo, &si,
                                                    sizeof(si)) &&
                       si.NumberOfLinks == 1;
        if (regular && SetSd(ini.h, kIniSddl)) {
            return rc;
        }
        DeleteByHandle(ini.h);
        ini.reset();
    }
    Sd iniSd(kIniSddl);
    Handle created = OpenRel(root.h, kIniName, GENERIC_WRITE, FILE_CREATE,
                             FILE_NON_DIRECTORY_FILE, iniSd.p, &st);
    if (!created) {
        return kErrIni;
    }
    std::string bytes = DefaultIniBytes(defaultIniPath);
    DWORD written = 0;
    if (!WriteFile(created.h, bytes.data(), (DWORD)bytes.size(), &written,
                   nullptr) ||
        written != bytes.size()) {
        return kErrIni;
    }
    return rc;
}

// --remove-datadir: xóa hẳn %ProgramData%\AmLichVN (khi gỡ cài đặt).
inline DWORD Remove() {
    if (!EnablePrivileges()) {
        return kErrNotAdmin;
    }
    Handle pd = OpenProgramData();
    if (!pd) {
        return kErrRemove;
    }
    return DeleteTreeRel(pd.h, kDirName) ? kOk : kErrRemove;
}

// --- Kiểm tra (chạy được với quyền thường) ----------------------------------

enum class State { Secure, Missing, Insecure };

// Thư mục / file chỉ an toàn khi: không phải reparse point, chủ sở hữu là
// Administrators, DACL được bảo vệ, và không ACE nào cho người không phải
// SYSTEM / Administrators các quyền trong `forbidden` trên chính đối tượng.
inline bool HasSafeSd(HANDLE h, bool wantDir, ACCESS_MASK forbidden) {
    DWORD attrs = 0;
    if (!GetAttrs(h, &attrs) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(attrs & FILE_ATTRIBUTE_DIRECTORY) != wantDir) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD rev = 0;
    bool ok = owner && dacl && GetSecurityDescriptorControl(sd, &control, &rev) &&
              (control & SE_DACL_PROTECTED) &&
              IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
    forbidden |= GENERIC_ALL | GENERIC_WRITE | MAXIMUM_ALLOWED;
    for (DWORD i = 0; ok && i < dacl->AceCount; i++) {
        ACE_HEADER* ace = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void**>(&ace))) {
            ok = false;
            break;
        }
        if (ace->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }
        if (ace->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            ok = false;  // loại ACE lạ: coi như không an toàn
            break;
        }
        auto* allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = &allowed->SidStart;
        if (IsWellKnownSid(sid, WinLocalSystemSid) ||
            IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
            continue;
        }
        if (allowed->Mask & forbidden) {
            ok = false;
        }
    }
    LocalFree(sd);
    return ok;
}

inline Handle OpenForCheck(const std::wstring& path) {
    return Handle(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr));
}

inline State Check() {
    std::wstring dir = DataDirPath();
    Handle root = OpenForCheck(dir);
    if (!root) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                   ? State::Missing
                   : State::Insecure;
    }
    constexpr ACCESS_MASK kDirWrite = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
                                      FILE_DELETE_CHILD | DELETE | WRITE_DAC |
                                      WRITE_OWNER;
    if (!HasSafeSd(root.h, true, kDirWrite)) {
        return State::Insecure;
    }
    Handle status = OpenForCheck(dir + L"\\" + kStatusName);
    if (!status ||
        !HasSafeSd(status.h, true,
                   FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE |
                       WRITE_DAC | WRITE_OWNER)) {
        return State::Insecure;
    }
    // settings.ini có thể chưa có (bộ cài sẽ tạo); nếu có thì phải đúng ACL.
    Handle ini = OpenForCheck(dir + L"\\" + kIniName);
    if (ini && !HasSafeSd(ini.h, false, DELETE | WRITE_DAC | WRITE_OWNER)) {
        return State::Insecure;
    }
    return State::Secure;
}

}  // namespace datadir
