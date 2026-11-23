// AmLichVN.exe - ứng dụng chạy nền (icon ở khay hệ thống):
//   * Nạp AmLichTap.dll vào tiến trình bảng lịch của Windows (ShellHost.exe /
//     ShellExperienceHost.exe) qua InitializeXamlDiagnosticsEx; tự nạp lại khi
//     tiến trình đó khởi động lại.
//   * Tự kiểm tra: đọc file trạng thái mà DLL ghi ra, phát hiện Windows vừa cập
//     nhật, và thông báo khi âm lịch không còn hoạt động.
//   * Icon khay hiện ngày âm lịch hôm nay.
//
// Tham số dòng lệnh:
//   --inject-once    nạp DLL một lần rồi thoát (không hiện icon)
//   --restart-shell  khởi động lại ShellHost/ShellExperienceHost (gỡ DLL) rồi thoát
//   --quit           đóng bản AmLichVN.exe đang chạy
//   --update-settings  mở cửa sổ "Cập nhật phần mềm"
//   --check-update   kiểm tra cập nhật một lần, ghi kết quả vào app-log.txt
//                    (%LOCALAPPDATA%\AmLichVN)
//                    (mã thoát: 0 = mới nhất, 10 = có bản mới, 1 = lỗi)
//   --apply-update <thư mục>  (nội bộ, chạy với quyền Administrator) kiểm tra
//                    chữ ký rồi cài bản cập nhật đã tải vào <thư mục>
//   --updated        (nội bộ) vừa cập nhật xong, hiện thông báo
//   --secure-datadir (quyền Administrator) tạo / sửa quyền %ProgramData%\AmLichVN
//                    (xem datadir.h); bộ cài gọi lệnh này
//   --remove-datadir (quyền Administrator) xóa %ProgramData%\AmLichVN khi gỡ cài
//
// GNU GPL v3.0.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <aclapi.h>
#include <appmodel.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <xamlom.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

#include "../common/shared.h"
#include "../common/vnlunar.h"
#include "datadir.h"
#include "updater.h"

// Giao diện điều khiển kiểu Windows hiện đại + TaskDialog (Common Controls 6).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

////////////////////////////////////////////////////////////////////////////////
// Tiện ích
////////////////////////////////////////////////////////////////////////////////

constexpr wchar_t kWindowClass[] = L"AmLichVN_TrayWindow";
constexpr wchar_t kAppName[] = L"Âm lịch Việt Nam";
constexpr wchar_t kRegKey[] = L"Software\\AmLichVN";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"AmLichVN";
constexpr PCWSTR kTargets[] = {L"ShellHost.exe", L"ShellExperienceHost.exe"};

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_INJECT_DONE = WM_APP + 2;
constexpr UINT WM_UPDATE_CHECKED = WM_APP + 3;  // lParam = upd::CheckResult*
constexpr UINT WM_UPDATE_APPLIED = WM_APP + 4;  // wParam = mã thoát
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 5000;

enum MenuId : UINT {
    ID_STATUS = 100,
    ID_REINJECT,
    ID_SETTINGS,
    ID_DATADIR,
    ID_AUTOSTART,
    ID_UNLOAD,
    ID_EXIT,
    ID_UPDATE_NOW,
    ID_CHECK_UPDATE,
    ID_UPDATE_SETTINGS,
    ID_SECURE_DATADIR,
};

HINSTANCE g_hInst;
HWND g_hWnd;
HANDLE g_mutex;
UINT g_taskbarCreatedMsg;
NOTIFYICONDATAW g_nid{};
HICON g_icon;
int g_iconDay = -1;
bool g_paused;  // người dùng đã chọn "Gỡ âm lịch khỏi lịch"
bool g_dataDirInsecure;  // %ProgramData%\AmLichVN chưa được bộ cài bảo vệ

std::wstring ExeDir() {
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L'\\'));
}

std::wstring ExePath() {
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return std::wstring(path, n);
}

// Đang chạy từ gói MSIX (Microsoft Store) hay bản cài thường?
bool IsPackaged() {
    static int packaged = -1;
    if (packaged < 0) {
        UINT32 len = 0;
        packaged = GetCurrentPackageFullName(&len, nullptr) ==
                           ERROR_INSUFFICIENT_BUFFER
                       ? 1
                       : 0;
    }
    return packaged == 1;
}

std::wstring LocalAppData() {
    WCHAR buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : L"";
}

// Bản MSIX: %LOCALAPPDATA%\Packages\<gói>\LocalState (đường dẫn thật, riêng
// từng người dùng). Chuỗi rỗng nếu không chạy trong gói.
std::wstring PackageLocalState() {
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 len = ARRAYSIZE(family);
    std::wstring local = LocalAppData();
    if (local.empty() || GetCurrentPackageFamilyName(&len, family) != ERROR_SUCCESS) {
        return L"";
    }
    return local + L"\\Packages\\" + family + L"\\LocalState";
}

// Thư mục dữ liệu dùng chung với AmLichTap.dll (settings.ini, status\).
//  - Bản cài thường: %ProgramData%\AmLichVN, do bộ cài (quyền Admin) tạo với
//    ACL được bảo vệ (xem datadir.h). Tiến trình thường không tự tạo thư mục
//    này, để người dùng khác không thể "chiếm chỗ" trước.
//  - Bản MSIX: <LocalState>\data, riêng từng người dùng.
std::wstring DataDir() {
    if (IsPackaged()) {
        std::wstring ls = PackageLocalState();
        if (!ls.empty()) {
            return ls + L"\\data";
        }
    }
    return datadir::DataDirPath();
}

// Log của AmLichVN.exe: riêng từng người dùng, không nằm trong thư mục dùng
// chung (%LOCALAPPDATA%\AmLichVN; bản MSIX: <LocalState>).
std::wstring LogDir() {
    if (IsPackaged()) {
        std::wstring ls = PackageLocalState();
        if (!ls.empty()) {
            return ls;
        }
    }
    std::wstring local = LocalAppData();
    return local.empty() ? L"" : local + L"\\AmLichVN";
}

std::wstring WindowsDir() {
    WCHAR buf[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(buf, ARRAYSIZE(buf));
    return (n && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : L"C:\\Windows";
}

const std::wstring& UserSid() {
    static const std::wstring sid = amlich::CurrentUserSidString();
    return sid;
}

std::wstring ReadOsBuild() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return L"unknown";
    }
    WCHAR build[32] = L"";
    DWORD size = sizeof(build);
    RegQueryValueExW(key, L"CurrentBuild", nullptr, nullptr, (BYTE*)build,
                     &size);
    DWORD ubr = 0;
    size = sizeof(ubr);
    RegQueryValueExW(key, L"UBR", nullptr, nullptr, (BYTE*)&ubr, &size);
    RegCloseKey(key);
    WCHAR buf[64];
    swprintf_s(buf, L"%ls.%lu", build, ubr);
    return buf;
}

std::wstring RegGetString(PCWSTR name) {
    WCHAR buf[1024] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr,
                     buf, &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

void RegSetString(PCWSTR name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, value.c_str(),
                    (DWORD)((value.size() + 1) * sizeof(WCHAR)));
}

void LogLine(const std::wstring& line) {
    OutputDebugStringW((L"[AmLichVN.exe] " + line + L"\r\n").c_str());
    std::wstring dir = LogDir();
    if (dir.empty()) {
        return;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\app-log.txt";

    // Không đi theo reparse point; kích thước lấy từ chính handle đã mở.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    // Giữ file log nhỏ: quá 256 KB thì ghi lại từ đầu.
    LARGE_INTEGER size{};
    LARGE_INTEGER zero{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 256 * 1024) {
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
    } else {
        SetFilePointerEx(file, zero, nullptr, FILE_END);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR ts[32];
    swprintf_s(ts, L"%04u-%02u-%02u %02u:%02u:%02u ", st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring w = ts + line + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), utf8.data(), n,
                        nullptr, nullptr);
    DWORD written;
    WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(file);
}

std::wstring HrToString(HRESULT hr) {
    WCHAR buf[32];
    swprintf_s(buf, L"0x%08X", (unsigned)hr);
    return buf;
}

bool IniBool(PCWSTR key, bool def) {
    std::wstring path = DataDir() + L"\\" AMLICH_SETTINGS_FILE;
    return GetPrivateProfileIntW(AMLICH_INI_SECTION, key, def ? 1 : 0,
                                 path.c_str()) != 0;
}

////////////////////////////////////////////////////////////////////////////////
// Tìm tiến trình đích
////////////////////////////////////////////////////////////////////////////////

struct TargetProcess {
    DWORD pid;
    std::wstring name;
};

std::vector<TargetProcess> FindTargets() {
    std::vector<TargetProcess> result;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return result;
    }
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            for (PCWSTR t : kTargets) {
                if (_wcsicmp(pe.szExeFile, t) == 0) {
                    DWORD session = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &session) &&
                        session == mySession) {
                        result.push_back({pe.th32ProcessID, t});
                    }
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

bool IsProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) {
        return false;
    }
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

////////////////////////////////////////////////////////////////////////////////
// Đọc file trạng thái do DLL ghi
////////////////////////////////////////////////////////////////////////////////

struct DllStatus {
    std::map<std::wstring, std::wstring> kv;
    std::wstring Get(const std::wstring& k) const {
        auto it = kv.find(k);
        return it == kv.end() ? L"" : it->second;
    }
    DWORD Pid() const { return (DWORD)wcstoul(Get(L"pid").c_str(), nullptr, 10); }
};

// Chỉ tin file trạng thái là file thường, một tên và do chính người dùng hiện
// tại sở hữu, tức là do AmLichTap.dll trong shell của người dùng này ghi. File
// của người khác (kể cả file "chiếm chỗ" trùng tên) bị bỏ qua.
bool ReadDllStatus(const std::wstring& processName, DllStatus* out) {
    std::wstring path = amlich::StatusFilePath(DataDir(), UserSid(), processName);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!amlich::IsOwnSingleLinkFile(file)) {
        CloseHandle(file);
        return false;
    }
    char buf[8192];
    DWORD read = 0;
    ReadFile(file, buf, sizeof(buf) - 1, &read, nullptr);
    CloseHandle(file);
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, text.data(), n);

    out->kv.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            out->kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        pos = end + 1;
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Nạp DLL (InitializeXamlDiagnosticsEx)
////////////////////////////////////////////////////////////////////////////////

using PFN_IXDE = decltype(&InitializeXamlDiagnosticsEx);

// Đường dẫn AmLichTap.dll mà tiến trình bảng lịch sẽ nạp. Gán một lần trong
// wWinMain (xem PrepareTapDll) trước khi có luồng nạp nào chạy.
std::wstring g_tapDllPath;

HRESULT InjectInto(DWORD pid) {
    const std::wstring& dllPath = g_tapDllPath;
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    static HMODULE wux = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wux) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    static auto ixde =
        reinterpret_cast<PFN_IXDE>(GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
    if (!ixde) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Thử lần lượt các tên kết nối cho đến khi gặp tên mà tiến trình đích
    // đang lắng nghe.
    HRESULT hr = E_FAIL;
    for (int i = 1; i <= 10000; i++) {
        WCHAR conn[64];
        swprintf_s(conn, L"VisualDiagConnection%d", i);
        hr = ixde(conn, pid, L"", dllPath.c_str(), CLSID_AmLichTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }
    return hr;
}

struct InjectState {
    std::wstring name;
    bool injected = false;
    bool inProgress = false;
    int attempts = 0;
    ULONGLONG nextAttempt = 0;
    HRESULT lastResult = S_FALSE;
};

std::mutex g_injectMutex;
std::map<DWORD, InjectState> g_inject;  // theo pid

struct InjectJob {
    DWORD pid;
};

// Nạp trên luồng riêng: nếu tiến trình đích đang bị tạm dừng (suspended),
// lệnh nạp có thể chờ đến khi nó chạy lại.
void StartInjectJob(DWORD pid) {
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            DWORD pid = (DWORD)(ULONG_PTR)param;
            HRESULT hr = InjectInto(pid);
            PostMessageW(g_hWnd, WM_INJECT_DONE, pid, (LPARAM)hr);
            return 0;
        },
        (LPVOID)(ULONG_PTR)pid, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        std::lock_guard<std::mutex> lock(g_injectMutex);
        g_inject[pid].inProgress = false;
    }
}

void CheckAndInject() {
    if (g_paused) {
        return;
    }
    auto targets = FindTargets();

    std::lock_guard<std::mutex> lock(g_injectMutex);

    // Bỏ các pid đã chết.
    for (auto it = g_inject.begin(); it != g_inject.end();) {
        bool found = std::any_of(targets.begin(), targets.end(),
                                 [&](const TargetProcess& t) {
                                     return t.pid == it->first;
                                 });
        if (!found && !it->second.inProgress) {
            it = g_inject.erase(it);
        } else {
            ++it;
        }
    }

    ULONGLONG now = GetTickCount64();
    for (const auto& t : targets) {
        auto& st = g_inject[t.pid];
        st.name = t.name;
        if (st.injected || st.inProgress || now < st.nextAttempt) {
            continue;
        }

        // AmLichVN.exe vừa khởi động lại mà DLL đã nằm sẵn trong tiến trình?
        DllStatus ds;
        if (st.attempts == 0 && ReadDllStatus(t.name, &ds) && ds.Pid() == t.pid) {
            st.injected = true;
            st.lastResult = S_OK;
            LogLine(L"DLL đã có sẵn trong " + t.name + L" (pid " +
                    std::to_wstring(t.pid) + L")");
            continue;
        }

        st.inProgress = true;
        st.attempts++;
        StartInjectJob(t.pid);
    }
}

void OnInjectDone(DWORD pid, HRESULT hr) {
    std::wstring name;
    int attempts;
    {
        std::lock_guard<std::mutex> lock(g_injectMutex);
        auto it = g_inject.find(pid);
        if (it == g_inject.end()) {
            return;
        }
        auto& st = it->second;
        st.inProgress = false;
        st.lastResult = hr;
        name = st.name;
        attempts = st.attempts;
        if (SUCCEEDED(hr)) {
            st.injected = true;
        } else {
            // Thử lại: dày trong phút đầu (shell có thể chưa khởi tạo xong
            // XAML), sau đó thưa dần.
            st.nextAttempt =
                GetTickCount64() + (attempts < 12 ? 5000 : 60000);
        }
    }
    LogLine(L"Nạp vào " + name + L" (pid " + std::to_wstring(pid) +
            L"), lần " + std::to_wstring(attempts) + L": " + HrToString(hr));
}

////////////////////////////////////////////////////////////////////////////////
// Tự kiểm tra & thông báo
////////////////////////////////////////////////////////////////////////////////

enum class Overall { Pending, Ok, Fail };

struct OverallStatus {
    Overall state = Overall::Pending;
    std::wstring detail;
};

std::wstring g_osBuild;
Overall g_lastOverall = Overall::Pending;
bool g_balloonIsUpdate = false;  // thông báo đang hiện là "có bản mới"

void ShowBalloon(const std::wstring& title,
                 const std::wstring& text,
                 DWORD icon = NIIF_INFO) {
    g_balloonIsUpdate = false;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

OverallStatus ComputeOverall() {
    OverallStatus result;
    bool anyFail = false;
    std::wstring failDetail;

    for (PCWSTR t : kTargets) {
        DllStatus ds;
        if (!ReadDllStatus(t, &ds)) {
            continue;
        }
        if (ds.Get(L"build") != g_osBuild || !IsProcessAlive(ds.Pid())) {
            continue;  // file cũ
        }
        std::wstring state = ds.Get(L"state");
        if (state == L"OK") {
            result.state = Overall::Ok;
            result.detail = ds.Get(L"detail");
            return result;
        }
        if (state == L"FAIL") {
            anyFail = true;
            failDetail = std::wstring(t) + L": " + ds.Get(L"detail");
        }
    }

    if (anyFail) {
        result.state = Overall::Fail;
        result.detail = failDetail;
        return result;
    }

    // Nạp DLL thất bại nhiều lần cũng là lỗi.
    std::lock_guard<std::mutex> lock(g_injectMutex);
    for (const auto& [pid, st] : g_inject) {
        if (!st.injected && st.attempts >= 12 && FAILED(st.lastResult)) {
            result.state = Overall::Fail;
            result.detail = L"Không nạp được AmLichTap.dll vào " + st.name +
                            L" (mã lỗi " + HrToString(st.lastResult) + L")";
            return result;
        }
    }

    result.detail = L"Chưa xác minh - hãy bấm vào đồng hồ để mở lịch một lần";
    return result;
}

void CheckStatusAndNotify() {
    OverallStatus os = ComputeOverall();

    if (os.state == Overall::Ok) {
        std::wstring lastGood = RegGetString(L"LastGoodBuild");
        if (lastGood != g_osBuild) {
            if (!lastGood.empty()) {
                ShowBalloon(kAppName, L"Đã kiểm tra sau cập nhật: âm lịch vẫn "
                                      L"hoạt động bình thường trên Windows build " +
                                          g_osBuild + L".");
            }
            RegSetString(L"LastGoodBuild", g_osBuild);
            LogLine(L"Xác minh OK trên build " + g_osBuild);
        }
    } else if (os.state == Overall::Fail && !g_paused) {
        if (IniBool(L"notifyOnError", true) &&
            RegGetString(L"NotifiedFailBuild") != g_osBuild) {
            RegSetString(L"NotifiedFailBuild", g_osBuild);
            ShowBalloon(L"Âm lịch không hoạt động",
                        os.detail +
                            L"\nThường do Windows vừa cập nhật. Bấm chuột phải "
                            L"vào icon > Trạng thái để xem chi tiết.",
                        NIIF_WARNING);
            LogLine(L"LỖI trên build " + g_osBuild + L": " + os.detail);
        }
    }
    g_lastOverall = os.state;
}

////////////////////////////////////////////////////////////////////////////////
// Icon khay: vẽ ngày âm lịch hôm nay
////////////////////////////////////////////////////////////////////////////////

vnlunar::LunarDate TodayLunar() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return vnlunar::SolarToLunar(st.wDay, st.wMonth, st.wYear);
}

HICON CreateDayIcon(int day) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size < 16) {
        size = 16;
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HGDIOBJ oldBmp = SelectObject(dc, color);

    // Nền đỏ son bo góc, chữ trắng.
    memset(bits, 0, size * size * 4);
    HBRUSH bg = CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
    HGDIOBJ oldBrush = SelectObject(dc, bg);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, 0, 0, size + 1, size + 1, size / 3, size / 3);

    std::wstring text = std::to_wstring(day);
    HFONT font = CreateFontW(-(int)(size * (text.size() > 1 ? 0.72 : 0.85)), 0,
                             0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc{0, 0, size, size};
    DrawTextW(dc, text.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    // GDI không ghi kênh alpha: điểm nào đã được vẽ thì cho đục.
    auto* px = (DWORD*)bits;
    for (int i = 0; i < size * size; i++) {
        if (px[i] & 0x00FFFFFF) {
            px[i] |= 0xFF000000;
        }
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(bg);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::wstring StatusShortText() {
    switch (g_lastOverall) {
        case Overall::Ok:
            return L"đang hoạt động";
        case Overall::Fail:
            return L"LỖI - xem Trạng thái";
        default:
            return g_paused ? L"đã tạm gỡ" : L"chờ mở lịch để kiểm tra";
    }
}

void UpdateTrayIcon(bool add) {
    vnlunar::LunarDate l = TodayLunar();
    if (l.day != g_iconDay || !g_icon) {
        HICON newIcon = CreateDayIcon(l.day);
        if (g_icon) {
            DestroyIcon(g_icon);
        }
        g_icon = newIcon;
        g_iconDay = l.day;
    }

    std::wstring tip = L"Âm lịch: " + std::to_wstring(l.day) + L"/" +
                       std::to_wstring(l.month) + (l.leap ? L" nhuận" : L"") +
                       L" " + vnlunar::YearCanChi(l.year) + L"\nTrạng thái: " +
                       StatusShortText();

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);

    if (add) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Menu & các lệnh
////////////////////////////////////////////////////////////////////////////////

// Bản MSIX: khóa Run trong registry bị ảo hóa nên dùng StartupTask khai báo
// trong AppxManifest.xml.
constexpr wchar_t kStartupTaskId[] = L"AmLichVNStartup";

winrt::Windows::ApplicationModel::StartupTask GetStartupTask() {
    return winrt::Windows::ApplicationModel::StartupTask::GetAsync(
               kStartupTaskId)
        .get();
}

bool IsAutostartEnabled() {
    if (IsPackaged()) {
        try {
            auto state = GetStartupTask().State();
            return state ==
                       winrt::Windows::ApplicationModel::StartupTaskState::
                           Enabled ||
                   state == winrt::Windows::ApplicationModel::
                                StartupTaskState::EnabledByPolicy;
        } catch (...) {
            return false;
        }
    }
    WCHAR buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ,
                        nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    if (IsPackaged()) {
        try {
            auto task = GetStartupTask();
            if (enable) {
                auto state = task.RequestEnableAsync().get();
                if (state == winrt::Windows::ApplicationModel::
                                 StartupTaskState::DisabledByUser) {
                    LogLine(L"Khởi động cùng Windows đang bị tắt trong "
                            L"Settings > Apps > Startup");
                }
            } else {
                task.Disable();
            }
        } catch (...) {
            LogLine(L"Không đổi được StartupTask: " +
                    HrToString(winrt::to_hresult()));
        }
        return;
    }
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ,
                        cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(WCHAR)));
    } else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            RegDeleteValueW(key, kRunValue);
            RegCloseKey(key);
        }
    }
}

// Tắt các tiến trình shell giữ DLL; Windows sẽ tự khởi động lại chúng.
int RestartShellProcesses() {
    int killed = 0;
    for (const auto& t : FindTargets()) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, t.pid);
        if (h) {
            if (TerminateProcess(h, 0)) {
                killed++;
            }
            CloseHandle(h);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_injectMutex);
        g_inject.clear();
    }
    LogLine(L"Đã khởi động lại " + std::to_wstring(killed) +
            L" tiến trình shell");
    return killed;
}

////////////////////////////////////////////////////////////////////////////////
// Cập nhật phần mềm qua GitHub Releases (có chữ ký số, luôn hỏi trước khi cài)
////////////////////////////////////////////////////////////////////////////////

// Chỉ tự kiểm tra một lần khi khởi động. Lúc mới bật máy có thể chưa có mạng
// nên nếu lỗi thì thử lại tối đa kAutoCheckMaxAttempts lần.
constexpr ULONGLONG kAutoCheckFirstDelay = 30ull * 1000;
constexpr ULONGLONG kAutoCheckRetry = 10ull * 60 * 1000;
constexpr int kAutoCheckMaxAttempts = 3;
constexpr ULONGLONG kNever = ~0ull;
constexpr int kBtnUpdate = 1001;
constexpr int kBtnLater = 1002;

struct UpdateUiState {
    bool checking = false;
    bool manualCheck = false;
    bool busy = false;  // đang hỏi / tải / cài
    ULONGLONG nextAutoCheck = kNever;
    int autoAttempts = 0;
    std::shared_ptr<upd::CheckResult> available;  // bản mới tìm được
    std::wstring balloonedVersion;
};
UpdateUiState g_upd;
HWND g_settingsWnd;

// Kho GitHub chứa bản phát hành (cố định).
std::wstring UpdateRepo() {
    return upd::kDefaultRepo;
}

bool UpdateAutoCheckEnabled() {
    return RegGetString(L"UpdateAutoCheck") != L"0";
}

std::wstring NowDisplayString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR buf[32];
    swprintf_s(buf, L"%02u/%02u/%04u %02u:%02u", st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute);
    return buf;
}

HWND DialogOwner() {
    return g_settingsWnd && IsWindowVisible(g_settingsWnd) ? g_settingsWnd
                                                           : g_hWnd;
}

void RefreshUpdateSettingsStatus();

using PFN_TaskDialogIndirect = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,
                                                int*,
                                                int*,
                                                BOOL*);

// Lấy TaskDialogIndirect động: nếu vì lý do nào đó không có Common Controls 6
// thì dùng MessageBox thay thế thay vì không chạy được.
PFN_TaskDialogIndirect GetTaskDialog() {
    static PFN_TaskDialogIndirect fn = [] {
        HMODULE m = LoadLibraryW(L"comctl32.dll");
        return m ? reinterpret_cast<PFN_TaskDialogIndirect>(
                       GetProcAddress(m, "TaskDialogIndirect"))
                 : nullptr;
    }();
    return fn;
}

TASKDIALOGCONFIG BaseTaskDialogConfig(HWND owner) {
    TASKDIALOGCONFIG c{sizeof(c)};
    c.hwndParent = owner;
    c.hInstance = g_hInst;
    c.pszWindowTitle = kAppName;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    if (owner && owner != g_hWnd) {
        c.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;
    }
    return c;
}

void MessageDlg(HWND owner,
                PCWSTR icon,
                const std::wstring& main,
                const std::wstring& content) {
    if (owner == g_hWnd) {
        SetForegroundWindow(g_hWnd);
    }
    if (auto td = GetTaskDialog()) {
        TASKDIALOGCONFIG c = BaseTaskDialogConfig(owner);
        c.pszMainIcon = icon;
        c.pszMainInstruction = main.c_str();
        c.pszContent = content.c_str();
        c.dwCommonButtons = TDCBF_OK_BUTTON;
        td(&c, nullptr, nullptr, nullptr);
        return;
    }
    UINT type = icon == TD_ERROR_ICON     ? MB_ICONERROR
                : icon == TD_WARNING_ICON ? MB_ICONWARNING
                                          : MB_ICONINFORMATION;
    MessageBoxW(owner, (main + L"\n\n" + content).c_str(), kAppName,
                MB_OK | type);
}

// --- Bảo vệ thư mục dữ liệu ----------------------------------------------------

// Bản cài từ trước phiên bản 1.3.0 để %ProgramData%\AmLichVN cho mọi người dùng
// toàn quyền (kể cả xóa / tạo junction). Chạy lại "AmLichVN.exe
// --secure-datadir" với quyền Administrator để sửa (bộ cài mới tự làm việc này).
void PromptSecureDataDir(bool afterUpdate) {
    if (IsPackaged()) {
        return;
    }
    if (auto td = GetTaskDialog()) {
        TASKDIALOGCONFIG c = BaseTaskDialogConfig(DialogOwner());
        c.pszMainIcon = TD_SHIELD_ICON;
        c.pszMainInstruction = L"Sửa quyền thư mục dữ liệu";
        std::wstring content =
            std::wstring(afterUpdate ? L"Bản cập nhật này siết lại quyền của thư mục "
                                     : L"Thư mục ") +
            DataDir() +
            L" đang cho mọi người dùng trên máy quyền sửa / xóa. Cần quyền "
            L"Administrator một lần để chỉ cho phép đọc và ghi đúng các file "
            L"cần thiết.\n\nCác file trạng thái và log cũ trong thư mục sẽ được "
            L"dọn; settings.ini được giữ nguyên.";
        c.pszContent = content.c_str();
        c.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
        c.nDefaultButton = IDYES;
        SetForegroundWindow(g_hWnd);
        int button = IDNO;
        if (FAILED(td(&c, &button, nullptr, nullptr)) || button != IDYES) {
            return;
        }
    } else if (MessageBoxW(g_hWnd,
                           L"Cần quyền Administrator để sửa quyền thư mục dữ "
                           L"liệu. Tiếp tục?",
                           kAppName, MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = DialogOwner();
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"--secure-datadir";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        if (GetLastError() != ERROR_CANCELLED) {
            MessageDlg(DialogOwner(), TD_ERROR_ICON, L"Không sửa được quyền",
                       upd::Win32ErrorText(GetLastError()));
        }
        return;
    }
    WaitForSingleObject(sei.hProcess, 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    g_dataDirInsecure = datadir::Check() != datadir::State::Secure;
    LogLine(L"--secure-datadir: mã " + std::to_wstring(code) +
            (g_dataDirInsecure ? L", thư mục vẫn chưa an toàn" : L", đã an toàn"));
    if (g_dataDirInsecure) {
        MessageDlg(DialogOwner(), TD_ERROR_ICON, L"Không sửa được quyền",
                   L"Mã lỗi " + std::to_wstring(code) +
                       L". Hãy chạy lại CaiDat.cmd để cài đặt lại.");
    } else {
        ShowBalloon(L"Đã sửa quyền thư mục dữ liệu", DataDir());
    }
}

// --- Kiểm tra ---------------------------------------------------------------

void StartUpdateCheck(bool manual) {
    if (IsPackaged()) {
        return;  // bản MSIX được cập nhật qua Microsoft Store
    }
    if (g_upd.checking) {
        g_upd.manualCheck |= manual;
        return;
    }
    if (g_upd.busy) {
        return;
    }
    auto* repo = new std::wstring(UpdateRepo());
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            std::unique_ptr<std::wstring> repo(static_cast<std::wstring*>(p));
            auto* r = new upd::CheckResult(upd::CheckForUpdate(*repo));
            if (!PostMessageW(g_hWnd, WM_UPDATE_CHECKED, 0, (LPARAM)r)) {
                delete r;
            }
            return 0;
        },
        repo, 0, nullptr);
    if (!thread) {
        delete repo;
        return;
    }
    CloseHandle(thread);
    g_upd.checking = true;
    g_upd.manualCheck = manual;
    RefreshUpdateSettingsStatus();
}

void PromptUpdate();

void OnUpdateChecked(upd::CheckResult* raw) {
    std::shared_ptr<upd::CheckResult> r(raw);
    bool manual = g_upd.manualCheck;
    g_upd.checking = false;
    g_upd.manualCheck = false;

    std::wstring latest = upd::Utf8ToWide(r->manifest.version);
    std::wstring summary;
    switch (r->kind) {
        case upd::CheckResult::Error:
            summary = L"Lỗi: " + r->error;
            break;
        case upd::CheckResult::UpToDate:
            summary = L"Đang dùng phiên bản mới nhất (bản phát hành mới nhất: " +
                      latest + L", chữ ký số hợp lệ).";
            g_upd.available.reset();
            break;
        case upd::CheckResult::Available:
            summary = L"Có phiên bản mới " + latest +
                      L" (chữ ký số hợp lệ).";
            g_upd.available = r;
            break;
    }
    if (summary.size() > 500) {
        summary.resize(500);
    }
    RegSetString(L"UpdateLastCheck", NowDisplayString());
    RegSetString(L"UpdateLastResult", summary);
    LogLine(L"Kiểm tra cập nhật (" + r->repo + L"): " + summary);

    if (!manual && r->kind == upd::CheckResult::Error &&
        g_upd.autoAttempts < kAutoCheckMaxAttempts) {
        g_upd.nextAutoCheck = GetTickCount64() + kAutoCheckRetry;
    } else {
        g_upd.nextAutoCheck = kNever;
    }
    RefreshUpdateSettingsStatus();

    if (manual) {
        if (r->kind == upd::CheckResult::Error) {
            MessageDlg(DialogOwner(), TD_ERROR_ICON,
                       L"Không kiểm tra được cập nhật", r->error);
        } else if (r->kind == upd::CheckResult::UpToDate) {
            MessageDlg(DialogOwner(), TD_INFORMATION_ICON,
                       L"Bạn đang dùng phiên bản mới nhất",
                       L"Phiên bản đang dùng: " AMLICH_VERSION
                       L"\nBản phát hành mới nhất trên GitHub: " +
                           latest + L" (chữ ký số hợp lệ).");
        } else {
            PromptUpdate();
        }
    } else if (r->kind == upd::CheckResult::Available &&
               latest != RegGetString(L"UpdateSkipVersion") &&
               latest != g_upd.balloonedVersion) {
        g_upd.balloonedVersion = latest;
        ShowBalloon(L"Có phiên bản mới: " + latest,
                    L"Bấm vào đây để xem và cập nhật Âm lịch Việt Nam.");
        g_balloonIsUpdate = true;
    }
}

void MaybeAutoCheckForUpdate() {
    if (IsPackaged() || g_upd.checking || g_upd.busy || GetTickCount64() < g_upd.nextAutoCheck ||
        !UpdateAutoCheckEnabled()) {
        return;
    }
    g_upd.nextAutoCheck = kNever;
    g_upd.autoAttempts++;
    StartUpdateCheck(false);
}

// --- Cài đặt ----------------------------------------------------------------

// Cài xong: khởi động lại tiến trình shell (để nhả DLL cũ) và chạy bản
// AmLichVN.exe mới thay cho bản này.
void FinishUpdateAndRestart() {
    LogLine(L"Cập nhật xong, khởi động lại AmLichVN");
    if (g_settingsWnd) {
        DestroyWindow(g_settingsWnd);
    }
    RestartShellProcesses();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
    std::wstring cmd = L"\"" + ExePath() + L"\" --updated";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LogLine(L"Không chạy được bản mới: " +
                upd::Win32ErrorText(GetLastError()));
    }
    DestroyWindow(g_hWnd);
}

void OnUpdateApplied(DWORD code) {
    // Tiến trình cài (quyền Admin) không ghi log; ghi kết quả ở đây.
    LogLine(L"Kết quả cài bản cập nhật: mã " + std::to_wstring(code));
    if (code == upd::kApplyOk) {
        FinishUpdateAndRestart();
        return;
    }
    g_upd.busy = false;
    RefreshUpdateSettingsStatus();
    std::wstring detail;
    switch (code) {
        case upd::kApplyBadSignature:
            detail = L"Chữ ký số hoặc mã SHA-256 của bản tải về không hợp lệ "
                     L"nên đã hủy cài đặt.";
            break;
        case upd::kApplyNotNewer:
            detail = L"Bản tải về không mới hơn bản đang cài.";
            break;
        case upd::kApplyIoError:
            detail = L"Không ghi được file vào thư mục cài đặt (có thể phần "
                     L"mềm diệt virus đang khóa file). Bản cũ được giữ nguyên.";
            break;
        default:
            detail = L"Lỗi khi cài đặt (mã " + std::to_wstring(code) + L").";
    }
    detail += L"\n\nChi tiết nằm trong app-log.txt (menu > Mở thư mục log).";
    MessageDlg(DialogOwner(), TD_ERROR_ICON, L"Cập nhật không thành công",
               detail);
}

void RunElevatedApply(const std::wstring& stagingDir) {
    std::wstring exe = ExePath();
    std::wstring params = L"--apply-update \"" + stagingDir + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = DialogOwner();
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        DWORD e = GetLastError();
        g_upd.busy = false;
        RefreshUpdateSettingsStatus();
        if (e == ERROR_CANCELLED) {
            LogLine(L"Người dùng hủy cấp quyền Administrator để cập nhật");
            MessageDlg(DialogOwner(), TD_INFORMATION_ICON, L"Đã hủy cập nhật",
                       L"Cần quyền Administrator để cài bản mới. Bạn có thể "
                       L"cập nhật lại bất cứ lúc nào từ menu của icon khay.");
        } else {
            MessageDlg(DialogOwner(), TD_ERROR_ICON,
                       L"Không chạy được trình cài đặt",
                       upd::Win32ErrorText(e));
        }
        return;
    }
    LogLine(L"Đang cài bản cập nhật từ " + stagingDir);
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            HANDLE process = static_cast<HANDLE>(p);
            WaitForSingleObject(process, INFINITE);
            DWORD code = upd::kApplyIoError;
            GetExitCodeProcess(process, &code);
            CloseHandle(process);
            PostMessageW(g_hWnd, WM_UPDATE_APPLIED, code, 0);
            return 0;
        },
        sei.hProcess, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = upd::kApplyIoError;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        OnUpdateApplied(code);
    }
}

struct DownloadJob {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    bool ok = false;
    std::wstring staging;
    std::wstring error;
    std::shared_ptr<upd::CheckResult> info;
};

HRESULT CALLBACK DownloadDialogProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM,
                                    LPARAM,
                                    LONG_PTR data) {
    auto* job = reinterpret_cast<DownloadJob*>(data);
    switch (msg) {
        case TDN_CREATED:
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
            break;
        case TDN_TIMER:
            if (job->done) {
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            }
            break;
        case TDN_BUTTON_CLICKED:
            if (!job->done) {
                job->cancel = true;
            }
            break;
    }
    return S_OK;
}

void StartDownloadAndInstall(std::shared_ptr<upd::CheckResult> info) {
    auto job = std::make_shared<DownloadJob>();
    job->info = info;
    auto* param = new std::shared_ptr<DownloadJob>(job);
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            std::unique_ptr<std::shared_ptr<DownloadJob>> holder(
                static_cast<std::shared_ptr<DownloadJob>*>(p));
            DownloadJob& j = **holder;
            j.ok = upd::DownloadUpdate(*j.info, &j.staging, &j.error, &j.cancel);
            j.done = true;
            return 0;
        },
        param, 0, nullptr);
    if (!thread) {
        delete param;
        g_upd.busy = false;
        return;
    }

    std::wstring version = upd::Utf8ToWide(info->manifest.version);
    std::wstring main = L"Đang tải phiên bản " + version + L"...";
    if (auto td = GetTaskDialog()) {
        TASKDIALOGCONFIG c = BaseTaskDialogConfig(DialogOwner());
        c.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszMainInstruction = main.c_str();
        c.pszContent = L"Tải từ GitHub và kiểm tra chữ ký số.";
        c.pfCallback = DownloadDialogProc;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(job.get());
        td(&c, nullptr, nullptr, nullptr);
    }
    if (!job->done) {
        job->cancel = true;  // hộp thoại đóng trước khi tải xong = hủy
    }
    WaitForSingleObject(thread, job->cancel ? 0 : INFINITE);
    CloseHandle(thread);

    if (job->cancel && !(job->done && job->ok)) {
        LogLine(L"Đã hủy tải bản cập nhật " + version);
        g_upd.busy = false;
        RefreshUpdateSettingsStatus();
        return;
    }
    if (!job->ok) {
        LogLine(L"Tải bản cập nhật thất bại: " + job->error);
        g_upd.busy = false;
        RefreshUpdateSettingsStatus();
        MessageDlg(DialogOwner(), TD_ERROR_ICON,
                   L"Không tải được bản cập nhật", job->error);
        return;
    }
    RunElevatedApply(job->staging);
}

HRESULT CALLBACK PromptDialogProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM,
                                  LPARAM lParam,
                                  LONG_PTR) {
    if (msg == TDN_CREATED) {
        SendMessageW(hwnd, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, kBtnUpdate,
                     TRUE);
    } else if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(hwnd, L"open", reinterpret_cast<PCWSTR>(lParam), nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

// Hỏi người dùng có muốn cập nhật lên bản mới đã tìm thấy không.
void PromptUpdate() {
    if (!g_upd.available || g_upd.busy) {
        return;
    }
    g_upd.busy = true;
    RefreshUpdateSettingsStatus();
    auto info = g_upd.available;
    std::wstring version = upd::Utf8ToWide(info->manifest.version);

    std::wstring main = L"Có phiên bản mới: " + version;
    std::wstring content =
        L"Bạn đang dùng phiên bản " AMLICH_VERSION L". Bản " + version +
        L" đã được xác minh chữ ký số của nhà phát triển.\n\nKhi cập nhật, "
        L"Windows sẽ hỏi quyền Administrator; bảng lịch và thông báo sẽ tắt vài "
        L"giây rồi tự mở lại.";
    std::wstring footer =
        L"<a href=\"" + upd::ReleasePageUrl(info->repo, info->manifest.tag) +
        L"\">Xem bản phát hành trên GitHub</a>";

    bool update = false;
    BOOL skip = FALSE;
    if (auto td = GetTaskDialog()) {
        TASKDIALOG_BUTTON buttons[] = {{kBtnUpdate, L"Cập nhật ngay"},
                                       {kBtnLater, L"Để sau"}};
        TASKDIALOGCONFIG c = BaseTaskDialogConfig(DialogOwner());
        c.dwFlags |= TDF_ENABLE_HYPERLINKS;
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = main.c_str();
        c.pszContent = content.c_str();
        c.pButtons = buttons;
        c.cButtons = ARRAYSIZE(buttons);
        c.nDefaultButton = kBtnUpdate;
        c.pszVerificationText = L"Không tự nhắc lại phiên bản này";
        c.pszFooter = footer.c_str();
        c.pszFooterIcon = TD_SHIELD_ICON;
        if (!info->manifest.notes.empty()) {
            c.pszExpandedInformation = info->manifest.notes.c_str();
            c.pszCollapsedControlText = L"Xem có gì mới";
            c.pszExpandedControlText = L"Ẩn ghi chú phát hành";
        }
        c.pfCallback = PromptDialogProc;
        if (DialogOwner() == g_hWnd) {
            SetForegroundWindow(g_hWnd);
        }
        int button = 0;
        if (SUCCEEDED(td(&c, &button, nullptr, &skip))) {
            update = button == kBtnUpdate;
        }
    } else {
        update = MessageBoxW(DialogOwner(),
                             (main + L"\n\n" + content + L"\n\nCập nhật ngay?")
                                 .c_str(),
                             kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    if (update) {
        LogLine(L"Người dùng đồng ý cập nhật lên " + version);
        StartDownloadAndInstall(info);
        return;
    }
    if (skip) {
        RegSetString(L"UpdateSkipVersion", version);
        LogLine(L"Bỏ qua nhắc phiên bản " + version);
    }
    g_upd.busy = false;
    RefreshUpdateSettingsStatus();
}

// --- Cửa sổ "Cài đặt" (hiển thị, chung, cập nhật) ---------------------------

void ShowStatusDialog();

constexpr wchar_t kSettingsClass[] = L"AmLichVN_Settings";
constexpr DWORD kSettingsStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr int kSettingsWidth = 760;   // DIP
constexpr int kSettingsHeight = 500;  // DIP

enum SettingsControl : int {
    IDC_TITLE = 200,
    IDC_SUBTITLE,
    IDC_GROUP_DISPLAY,
    IDC_FORMAT_LABEL,
    IDC_FORMAT,
    IDC_FONTSIZE_LABEL,
    IDC_FONTSIZE,
    IDC_FONTSIZE_UD,
    IDC_OPACITY_LABEL,
    IDC_OPACITY,
    IDC_OPACITY_UD,
    IDC_DAYFONT_LABEL,
    IDC_DAYFONT,
    IDC_DAYFONT_UD,
    IDC_MARGIN_LABEL,
    IDC_MARGIN,
    IDC_MARGIN_UD,
    IDC_MOVEUP,
    IDC_HIGHLIGHT,
    IDC_COLOR_SWATCH,
    IDC_COLOR_PICK,
    IDC_SOLARTERM,
    IDC_TOOLTIP,
    IDC_DAYINFO,
    IDC_GROUP_GENERAL,
    IDC_AUTOSTART,
    IDC_NOTIFY,
    IDC_DEBUGLOG,
    IDC_GROUP_UPDATE,
    IDC_AUTOCHECK,
    IDC_STATUS,
    IDC_CHECKNOW,
    IDC_RELEASES,
    IDC_OPEN_INI,
    IDC_SHOW_STATUS,
    IDC_APPLY,
    // IDOK = Lưu, IDCANCEL = Đóng
};

// Ô số trong settings.ini (giới hạn giống AmLichTap.dll).
struct NumberField {
    int label, edit, updown;
    int minValue, maxValue;
    PCWSTR key;
    int def;
    PCWSTR text;
};
constexpr NumberField kNumberFields[] = {
    {IDC_FONTSIZE_LABEL, IDC_FONTSIZE, IDC_FONTSIZE_UD, 6, 30, L"fontSize", 10,
     L"Cỡ chữ âm lịch (6 - 30):"},
    {IDC_OPACITY_LABEL, IDC_OPACITY, IDC_OPACITY_UD, 10, 100, L"opacity", 80,
     L"Độ đậm chữ âm lịch, % (10 - 100):"},
    {IDC_DAYFONT_LABEL, IDC_DAYFONT, IDC_DAYFONT_UD, 0, 40,
     L"dayNumberFontSize", 0, L"Cỡ chữ ngày dương (0 = mặc định):"},
    {IDC_MARGIN_LABEL, IDC_MARGIN, IDC_MARGIN_UD, 0, 30, L"bottomMargin", 1,
     L"Khoảng cách tới đáy ô, px (0 - 30):"},
};

// Ô đánh dấu trong settings.ini.
struct CheckField {
    int id;
    PCWSTR key;
    bool def;
};
constexpr CheckField kCheckFields[] = {
    {IDC_MOVEUP, L"moveDayNumberUp", true},
    {IDC_HIGHLIGHT, L"highlightSpecialDays", true},
    {IDC_SOLARTERM, L"showSolarTerm", false},
    {IDC_TOOLTIP, L"showTooltip", true},
    {IDC_DAYINFO, L"showDayInfo", true},
    {IDC_NOTIFY, L"notifyOnError", true},
    {IDC_DEBUGLOG, L"debugLog", false},
};

struct SettingsUi {
    UINT dpi = 96;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    COLORREF highlight = RGB(0xE0, 0x40, 0x40);
    HBRUSH swatchBrush = nullptr;
};
SettingsUi g_sui;

std::wstring SettingsIniPath() {
    return DataDir() + L"\\" AMLICH_SETTINGS_FILE;
}

// Mở file bằng Notepad trong System32 (đường dẫn đầy đủ, không tìm theo PATH).
void OpenInNotepad(HWND owner, const std::wstring& file) {
    WCHAR sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, ARRAYSIZE(sys));
    if (!n || n >= ARRAYSIZE(sys)) {
        return;
    }
    std::wstring notepad = std::wstring(sys, n) + L"\\notepad.exe";
    std::wstring args = L"\"" + file + L"\"";
    ShellExecuteW(owner, L"open", notepad.c_str(), args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

int IniInt(PCWSTR key, int def) {
    return (int)GetPrivateProfileIntW(AMLICH_INI_SECTION, key, def,
                                      SettingsIniPath().c_str());
}

std::wstring IniString(PCWSTR key, PCWSTR def) {
    WCHAR buf[128] = L"";
    GetPrivateProfileStringW(AMLICH_INI_SECTION, key, def, buf, ARRAYSIZE(buf),
                             SettingsIniPath().c_str());
    return buf;
}

bool IniWrite(PCWSTR key, const std::wstring& value) {
    // Không ghi qua reparse point (settings.ini dùng chung cho mọi người dùng).
    DWORD attrs = GetFileAttributesW(SettingsIniPath().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    return WritePrivateProfileStringW(AMLICH_INI_SECTION, key, value.c_str(),
                                      SettingsIniPath().c_str()) != FALSE;
}

bool ParseColor(std::wstring s, COLORREF* out) {
    if (!s.empty() && s[0] == L'#') {
        s.erase(0, 1);
    }
    if (s.size() == 8) {
        s.erase(0, 2);  // bỏ kênh alpha
    }
    if (s.size() != 6 ||
        s.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) {
        return false;
    }
    unsigned long v = wcstoul(s.c_str(), nullptr, 16);
    *out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

int Dip(int v) {
    return MulDiv(v, (int)g_sui.dpi, 96);
}

void ApplySettingsFonts(HWND hwnd) {
    if (g_sui.font) {
        DeleteObject(g_sui.font);
    }
    if (g_sui.titleFont) {
        DeleteObject(g_sui.titleFont);
    }
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                               g_sui.dpi);
    g_sui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = MulDiv(title.lfHeight, 3, 2);
    title.lfWeight = FW_SEMIBOLD;
    g_sui.titleFont = CreateFontIndirectW(&title);

    for (HWND child = GetWindow(hwnd, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        HFONT f = GetDlgCtrlID(child) == IDC_TITLE ? g_sui.titleFont : g_sui.font;
        SendMessageW(child, WM_SETFONT, (WPARAM)f, TRUE);
    }
}

void LayoutSettings(HWND hwnd) {
    const int W = kSettingsWidth, M = 20, gap = 16;
    const int colW = (W - 2 * M - gap) / 2;
    const int pad = 14, inner = colW - 2 * pad;
    auto place = [&](int id, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(hwnd, id), nullptr, Dip(x), Dip(y), Dip(w),
                     Dip(h), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    place(IDC_TITLE, M, 12, W - 2 * M, 30);
    place(IDC_SUBTITLE, M, 44, W - 2 * M, 20);

    // Cột trái: hiển thị.
    int top = 74, x = M + pad, y = top + 26;
    place(IDC_FORMAT_LABEL, x, y + 4, 104, 20);
    place(IDC_FORMAT, x + 104, y, inner - 104, 200);
    y += 36;
    for (const auto& f : kNumberFields) {
        place(f.label, x, y + 4, inner - 84, 20);
        place(f.edit, x + inner - 76, y, 76, 26);
        SendMessageW(GetDlgItem(hwnd, f.updown), UDM_SETBUDDY,
                     (WPARAM)GetDlgItem(hwnd, f.edit), 0);
        y += 32;
    }
    y += 2;
    place(IDC_MOVEUP, x, y, inner, 24);
    y += 28;
    place(IDC_HIGHLIGHT, x, y, inner - 132, 24);
    place(IDC_COLOR_SWATCH, x + inner - 128, y + 2, 26, 20);
    place(IDC_COLOR_PICK, x + inner - 96, y - 2, 96, 28);
    y += 30;
    place(IDC_SOLARTERM, x, y, inner, 24);
    y += 28;
    place(IDC_TOOLTIP, x, y, inner, 24);
    y += 28;
    place(IDC_DAYINFO, x, y, inner, 24);
    y += 28;
    place(IDC_GROUP_DISPLAY, M, top, colW, y - top + 8);
    int leftBottom = y + 8;

    // Cột phải: chung + cập nhật.
    int rx = M + colW + gap;
    x = rx + pad;
    y = top + 26;
    place(IDC_AUTOSTART, x, y, inner, 24);
    y += 28;
    place(IDC_NOTIFY, x, y, inner, 24);
    y += 28;
    place(IDC_DEBUGLOG, x, y, inner, 24);
    y += 28;
    place(IDC_GROUP_GENERAL, rx, top, colW, y - top + 8);

    top = y + 20;
    y = top + 26;
    if (!IsPackaged()) {  // bản Store không có ô tự kiểm tra
        place(IDC_AUTOCHECK, x, y, inner, 24);
        y += 30;
    }
    place(IDC_STATUS, x, y, inner, 72);
    y += 78;
    place(IDC_CHECKNOW, x, y, 124, 30);
    place(IDC_RELEASES, x + 132, y, 150, 30);
    y += 38;
    place(IDC_GROUP_UPDATE, rx, top, colW, y - top);

    // Hàng nút dưới cùng.
    int by = (std::max)(leftBottom, y) + 16;
    place(IDC_OPEN_INI, M, by, 130, 30);
    place(IDC_SHOW_STATUS, M + 138, by, 110, 30);
    place(IDCANCEL, W - M - 84, by, 84, 30);
    place(IDOK, W - M - 84 - 8 - 84, by, 84, 30);
    place(IDC_APPLY, W - M - 84 - 8 - 84 - 8 - 84, by, 84, 30);
}

void RefreshUpdateSettingsStatus() {
    if (!g_settingsWnd) {
        return;
    }
    std::wstring s = L"Phiên bản đang dùng: " AMLICH_VERSION L"\n";
    if (IsPackaged()) {
        s += L"Bản cài từ Microsoft Store được cập nhật tự động qua Store.";
    } else if (g_upd.checking) {
        s += L"Đang kiểm tra cập nhật...";
    } else if (g_upd.busy) {
        s += L"Đang cập nhật...";
    } else {
        std::wstring last = RegGetString(L"UpdateLastCheck");
        s += last.empty() ? L"Chưa kiểm tra lần nào."
                          : L"Lần kiểm tra gần nhất: " + last + L"\n" +
                                RegGetString(L"UpdateLastResult");
    }
    SetDlgItemTextW(g_settingsWnd, IDC_STATUS, s.c_str());
    EnableWindow(GetDlgItem(g_settingsWnd, IDC_CHECKNOW),
                 !g_upd.checking && !g_upd.busy);
    if (IsPackaged()) {
        for (int id : {IDC_AUTOCHECK, IDC_CHECKNOW, IDC_RELEASES}) {
            ShowWindow(GetDlgItem(g_settingsWnd, id), SW_HIDE);
        }
    }
}

bool SaveUpdateSettings(HWND hwnd) {
    bool autoCheck = IsDlgButtonChecked(hwnd, IDC_AUTOCHECK) == BST_CHECKED;
    RegSetString(L"UpdateAutoCheck", autoCheck ? L"1" : L"0");
    RefreshUpdateSettingsStatus();
    return true;
}

void SetSwatchColor(HWND hwnd, COLORREF color) {
    g_sui.highlight = color;
    if (g_sui.swatchBrush) {
        DeleteObject(g_sui.swatchBrush);
    }
    g_sui.swatchBrush = CreateSolidBrush(color);
    InvalidateRect(GetDlgItem(hwnd, IDC_COLOR_SWATCH), nullptr, TRUE);
}

void UpdateDependentControls(HWND hwnd) {
    bool highlight = IsDlgButtonChecked(hwnd, IDC_HIGHLIGHT) == BST_CHECKED;
    EnableWindow(GetDlgItem(hwnd, IDC_COLOR_PICK), highlight);
}

// Đọc settings.ini + registry vào các ô.
void LoadSettingsToUi(HWND hwnd) {
    SendDlgItemMessageW(hwnd, IDC_FORMAT, CB_SETCURSEL,
                        IniString(L"format", L"dayOnly") == L"dayMonth" ? 1 : 0,
                        0);
    for (const auto& f : kNumberFields) {
        int v = IniInt(f.key, f.def);
        bool valid = v >= f.minValue && v <= f.maxValue &&
                     !(f.edit == IDC_DAYFONT && v > 0 && v < 6);
        if (!valid) {
            v = f.def;
        }
        SendDlgItemMessageW(hwnd, f.updown, UDM_SETPOS32, 0, v);
        SetDlgItemInt(hwnd, f.edit, v, FALSE);
    }
    for (const auto& c : kCheckFields) {
        CheckDlgButton(hwnd, c.id,
                       IniInt(c.key, c.def ? 1 : 0) != 0 ? BST_CHECKED
                                                         : BST_UNCHECKED);
    }
    COLORREF color = RGB(0xE0, 0x40, 0x40);
    ParseColor(IniString(L"highlightColor", L"#E04040"), &color);
    SetSwatchColor(hwnd, color);
    CheckDlgButton(hwnd, IDC_AUTOSTART,
                   IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_AUTOCHECK,
                   UpdateAutoCheckEnabled() ? BST_CHECKED : BST_UNCHECKED);
    UpdateDependentControls(hwnd);
}

// Kiểm tra rồi ghi tất cả cài đặt. Trả về false nếu có ô không hợp lệ.
bool SaveAllSettings(HWND hwnd) {
    int values[ARRAYSIZE(kNumberFields)];
    for (size_t i = 0; i < ARRAYSIZE(kNumberFields); i++) {
        const auto& f = kNumberFields[i];
        BOOL ok = FALSE;
        int v = (int)GetDlgItemInt(hwnd, f.edit, &ok, FALSE);
        bool valid = ok && v >= f.minValue && v <= f.maxValue &&
                     !(f.edit == IDC_DAYFONT && v > 0 && v < 6);
        if (!valid) {
            std::wstring range =
                f.edit == IDC_DAYFONT
                    ? std::wstring(L"0 (mặc định) hoặc từ 6 đến 40")
                    : L"từ " + std::to_wstring(f.minValue) + L" đến " +
                          std::to_wstring(f.maxValue);
            MessageDlg(hwnd, TD_WARNING_ICON, L"Giá trị không hợp lệ",
                       std::wstring(f.text) + L"\nHãy nhập số " + range + L".");
            HWND edit = GetDlgItem(hwnd, f.edit);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return false;
        }
        values[i] = v;
    }
    if (!SaveUpdateSettings(hwnd)) {
        return false;
    }

    bool ok = true;
    int format = (int)SendDlgItemMessageW(hwnd, IDC_FORMAT, CB_GETCURSEL, 0, 0);
    ok &= IniWrite(L"format", format == 1 ? L"dayMonth" : L"dayOnly");
    for (size_t i = 0; i < ARRAYSIZE(kNumberFields); i++) {
        ok &= IniWrite(kNumberFields[i].key, std::to_wstring(values[i]));
    }
    for (const auto& c : kCheckFields) {
        ok &= IniWrite(c.key,
                       IsDlgButtonChecked(hwnd, c.id) == BST_CHECKED ? L"1"
                                                                     : L"0");
    }
    WCHAR color[16];
    swprintf_s(color, L"#%02X%02X%02X", GetRValue(g_sui.highlight),
               GetGValue(g_sui.highlight), GetBValue(g_sui.highlight));
    ok &= IniWrite(L"highlightColor", color);

    bool autostart = IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED;
    if (autostart != IsAutostartEnabled()) {
        SetAutostart(autostart);
    }

    if (!ok) {
        MessageDlg(hwnd, TD_ERROR_ICON, L"Không lưu được cài đặt",
                   L"Không ghi được " + SettingsIniPath() + L"\n" +
                       upd::Win32ErrorText(GetLastError()));
        return false;
    }
    LogLine(L"Đã lưu cài đặt");
    return true;
}

void PickHighlightColor(HWND hwnd) {
    static COLORREF custom[16] = {RGB(0xE0, 0x40, 0x40), RGB(0xC4, 0x2B, 0x1C),
                                  RGB(0xD8, 0x8C, 0x00), RGB(0x00, 0x78, 0xD4)};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = hwnd;
    cc.rgbResult = g_sui.highlight;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        SetSwatchColor(hwnd, cc.rgbResult);
    }
}

void CreateSettingsControls(HWND hwnd) {
    auto make = [&](PCWSTR cls, PCWSTR text, DWORD style, int id,
                    DWORD exStyle = 0) {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, g_hInst,
                               nullptr);
    };
    auto check = [&](PCWSTR text, int id) {
        make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id);
    };
    make(L"STATIC", L"Cài đặt Âm lịch Việt Nam", SS_NOPREFIX, IDC_TITLE);
    make(L"STATIC",
         L"Thay đổi về hiển thị có hiệu lực khi bạn mở lại bảng lịch (bấm vào "
         L"đồng hồ trên taskbar).",
         SS_NOPREFIX, IDC_SUBTITLE);

    // Hiển thị
    make(L"BUTTON", L"Hiển thị trên bảng lịch", BS_GROUPBOX, IDC_GROUP_DISPLAY);
    make(L"STATIC", L"Kiểu hiển thị:", SS_NOPREFIX, IDC_FORMAT_LABEL);
    HWND combo = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                      IDC_FORMAT);
    SendMessageW(combo, CB_ADDSTRING, 0,
                 (LPARAM)L"Chỉ ngày âm (mùng 1 hiện cả tháng)");
    SendMessageW(combo, CB_ADDSTRING, 0,
                 (LPARAM)L"Luôn hiện ngày/tháng (vd. 14/10)");
    for (const auto& f : kNumberFields) {
        make(L"STATIC", f.text, SS_NOPREFIX, f.label);
        make(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_RIGHT, f.edit,
             WS_EX_CLIENTEDGE);
        HWND ud = make(UPDOWN_CLASSW, nullptr,
                       UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                           UDS_NOTHOUSANDS,
                       f.updown);
        SendMessageW(ud, UDM_SETRANGE32, f.minValue, f.maxValue);
    }
    check(L"Đẩy số ngày dương lên phía trên ô", IDC_MOVEUP);
    check(L"Tô màu mùng 1, rằm và ngày lễ", IDC_HIGHLIGHT);
    make(L"STATIC", L"", WS_BORDER, IDC_COLOR_SWATCH);
    make(L"BUTTON", L"Chọn màu...", WS_TABSTOP | BS_PUSHBUTTON, IDC_COLOR_PICK);
    check(L"Hiện tên tiết khí vào ngày bắt đầu tiết", IDC_SOLARTERM);
    check(L"Rê chuột lên một ngày để xem chi tiết", IDC_TOOLTIP);
    check(L"Hiện khung thông tin ngày dưới bảng lịch", IDC_DAYINFO);

    // Chung
    make(L"BUTTON", L"Chung", BS_GROUPBOX, IDC_GROUP_GENERAL);
    check(L"Khởi động cùng Windows", IDC_AUTOSTART);
    check(L"Thông báo khi âm lịch không hoạt động", IDC_NOTIFY);
    check(L"Ghi log chi tiết (để tìm lỗi)", IDC_DEBUGLOG);

    // Cập nhật
    make(L"BUTTON", L"Cập nhật phần mềm", BS_GROUPBOX, IDC_GROUP_UPDATE);
    check(L"Tự động kiểm tra cập nhật mỗi khi khởi động", IDC_AUTOCHECK);
    make(L"STATIC", L"", SS_NOPREFIX, IDC_STATUS);
    make(L"BUTTON", L"Kiểm tra ngay", WS_TABSTOP | BS_PUSHBUTTON, IDC_CHECKNOW);
    make(L"BUTTON", L"Mở trang Releases", WS_TABSTOP | BS_PUSHBUTTON,
         IDC_RELEASES);

    // Nút dưới cùng
    make(L"BUTTON", L"Mở settings.ini", WS_TABSTOP | BS_PUSHBUTTON,
         IDC_OPEN_INI);
    make(L"BUTTON", L"Trạng thái...", WS_TABSTOP | BS_PUSHBUTTON,
         IDC_SHOW_STATUS);
    make(L"BUTTON", L"Áp dụng", WS_TABSTOP | BS_PUSHBUTTON, IDC_APPLY);
    make(L"BUTTON", L"Lưu", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    make(L"BUTTON", L"Đóng", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd,
                                 UINT msg,
                                 WPARAM wParam,
                                 LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_sui.dpi = GetDpiForWindow(hwnd);
            CreateSettingsControls(hwnd);
            ApplySettingsFonts(hwnd);
            LayoutSettings(hwnd);
            LoadSettingsToUi(hwnd);
            return 0;
        case WM_DPICHANGED: {
            g_sui.dpi = HIWORD(wParam);
            ApplySettingsFonts(hwnd);
            LayoutSettings(hwnd);
            auto* rc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            int id = GetDlgCtrlID((HWND)lParam);
            if (id == IDC_COLOR_SWATCH && g_sui.swatchBrush) {
                return (LRESULT)g_sui.swatchBrush;
            }
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(id == IDC_SUBTITLE ? COLOR_GRAYTEXT
                                                            : COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_HIGHLIGHT:
                    UpdateDependentControls(hwnd);
                    return 0;
                case IDC_COLOR_PICK:
                    PickHighlightColor(hwnd);
                    return 0;
                case IDC_RELEASES:
                    ShellExecuteW(hwnd, L"open",
                                  upd::ReleasePageUrl(UpdateRepo()).c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case IDC_CHECKNOW:
                    if (SaveUpdateSettings(hwnd)) {
                        StartUpdateCheck(true);
                    }
                    return 0;
                case IDC_OPEN_INI:
                    OpenInNotepad(hwnd, SettingsIniPath());
                    return 0;
                case IDC_SHOW_STATUS:
                    ShowStatusDialog();
                    return 0;
                case IDC_APPLY:
                    SaveAllSettings(hwnd);
                    return 0;
                case IDOK:
                    if (SaveAllSettings(hwnd)) {
                        DestroyWindow(hwnd);
                    }
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_settingsWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OpenSettingsWindow() {
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_RESTORE);
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kSettingsClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // Đặt cửa sổ giữa màn hình đang có con trỏ chuột.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    HWND hwnd = CreateWindowExW(0, kSettingsClass, L"Cài đặt - Âm lịch Việt Nam",
                                kSettingsStyle, mi.rcWork.left, mi.rcWork.top,
                                100, 100, nullptr, nullptr, g_hInst, nullptr);
    if (!hwnd) {
        return;
    }
    g_settingsWnd = hwnd;
    UINT dpi = GetDpiForWindow(hwnd);
    RECT rc{0, 0, MulDiv(kSettingsWidth, dpi, 96),
            MulDiv(kSettingsHeight, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, kSettingsStyle, FALSE, 0, dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    SetWindowPos(hwnd, nullptr, x, (std::max)(y, (int)mi.rcWork.top), w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
    }
    RefreshUpdateSettingsStatus();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, IDC_FORMAT));
}

void ShowStatusDialog() {
    vnlunar::LunarDate l = TodayLunar();
    std::wstring s;
    s += L"Hôm nay - " + amlich::BuildDescription(l) + L"\n\n";
    s += L"Phiên bản AmLichVN: " AMLICH_VERSION L"\n";
    s += L"Windows build: " + g_osBuild + L"\n";
    s += L"Build đã xác minh gần nhất: " + RegGetString(L"LastGoodBuild") +
         L"\n";
    s += L"Trạng thái chung: " + StatusShortText() + L"\n\n";

    {
        std::lock_guard<std::mutex> lock(g_injectMutex);
        for (const auto& [pid, st] : g_inject) {
            s += L"Nạp vào " + st.name + L" (pid " + std::to_wstring(pid) +
                 L"): " +
                 (st.injected ? L"đã nạp"
                  : st.inProgress
                      ? L"đang nạp..."
                      : L"chưa (lần thử " + std::to_wstring(st.attempts) +
                            L", mã " + HrToString(st.lastResult) + L")") +
                 L"\n";
        }
    }

    for (PCWSTR t : kTargets) {
        DllStatus ds;
        if (ReadDllStatus(t, &ds)) {
            s += L"\n[" + std::wstring(t) + L"] " + ds.Get(L"state") + L" - " +
                 ds.Get(L"detail") + L"\n   lúc " + ds.Get(L"time") +
                 L", build " + ds.Get(L"build") + L", pid " + ds.Get(L"pid") +
                 L", ô ngày: " + ds.Get(L"dayItems") + L", lỗi: " +
                 ds.Get(L"errors") + L"\n";
            if (!ds.Get(L"lastError").empty()) {
                s += L"   lỗi cuối: " + ds.Get(L"lastError") + L"\n";
            }
        }
    }
    MessageBoxW(g_hWnd, s.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
}

void ShowMenu() {
    vnlunar::LunarDate l = TodayLunar();
    std::wstring header = L"Hôm nay: " + std::to_wstring(l.day) + L"/" +
                          std::to_wstring(l.month) + L" âm lịch, năm " +
                          vnlunar::YearCanChi(l.year);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Cài đặt...");
    SetMenuDefaultItem(menu, ID_SETTINGS, FALSE);  // = bấm chuột trái
    AppendMenuW(menu, MF_STRING, ID_STATUS, L"Trạng thái...");
    AppendMenuW(menu, MF_STRING, ID_REINJECT, L"Nạp lại vào lịch");
    AppendMenuW(menu, MF_STRING, ID_DATADIR, L"Mở thư mục log");
    if (g_dataDirInsecure) {
        AppendMenuW(menu, MF_STRING, ID_SECURE_DATADIR,
                    L"Sửa quyền thư mục dữ liệu (cần quyền Admin)...");
    }
    AppendMenuW(menu,
                MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                ID_AUTOSTART, L"Khởi động cùng Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (g_upd.available) {
        std::wstring text = L"Cập nhật lên phiên bản " +
                            upd::Utf8ToWide(g_upd.available->manifest.version) +
                            L"...";
        AppendMenuW(menu, MF_STRING | (g_upd.busy ? MF_GRAYED : 0),
                    ID_UPDATE_NOW, text.c_str());
    }
    if (!IsPackaged()) {
        AppendMenuW(menu,
                    MF_STRING | (g_upd.checking || g_upd.busy ? MF_GRAYED : 0),
                    ID_CHECK_UPDATE,
                    g_upd.checking ? L"Đang kiểm tra cập nhật..."
                                   : L"Kiểm tra cập nhật");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_UNLOAD,
                L"Gỡ âm lịch khỏi lịch (khởi động lại shell)");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Thoát");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hWnd, nullptr);
    PostMessageW(g_hWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void OnCommand(UINT id) {
    switch (id) {
        case ID_STATUS:
            ShowStatusDialog();
            break;
        case ID_REINJECT: {
            g_paused = false;
            {
                std::lock_guard<std::mutex> lock(g_injectMutex);
                for (auto& [pid, st] : g_inject) {
                    if (!st.inProgress) {
                        st.injected = false;
                        st.attempts = 1;  // bỏ qua bước "đã có sẵn"
                        st.nextAttempt = 0;
                    }
                }
            }
            CheckAndInject();
            UpdateTrayIcon(false);
            break;
        }
        case ID_SETTINGS:
            OpenSettingsWindow();
            break;
        case ID_DATADIR: {
            // Mở bằng explorer.exe (đường dẫn đầy đủ), không để ShellExecute
            // tự chọn cách "mở" một đường dẫn.
            std::wstring dir = LogDir();
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring explorer = WindowsDir() + L"\\explorer.exe";
            std::wstring args = L"\"" + dir + L"\"";
            ShellExecuteW(g_hWnd, L"open", explorer.c_str(), args.c_str(),
                          nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_SECURE_DATADIR:
            PromptSecureDataDir(false);
            break;
        case ID_AUTOSTART:
            SetAutostart(!IsAutostartEnabled());
            if (g_settingsWnd) {
                CheckDlgButton(g_settingsWnd, IDC_AUTOSTART,
                               IsAutostartEnabled() ? BST_CHECKED
                                                    : BST_UNCHECKED);
            }
            break;
        case ID_UPDATE_NOW:
            PromptUpdate();
            break;
        case ID_CHECK_UPDATE:
            StartUpdateCheck(true);
            break;
        case ID_UPDATE_SETTINGS:
            OpenSettingsWindow();
            break;
        case ID_UNLOAD:
            if (MessageBoxW(g_hWnd,
                            L"Khởi động lại tiến trình bảng lịch/thông báo của "
                            L"Windows để gỡ âm lịch?\n(Bảng thông báo sẽ tắt "
                            L"vài giây rồi tự chạy lại.)\n\nChọn \"Nạp lại vào "
                            L"lịch\" để bật lại.",
                            kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_paused = true;
                RestartShellProcesses();
                UpdateTrayIcon(false);
            }
            break;
        case ID_EXIT:
            DestroyWindow(g_hWnd);
            break;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        UpdateTrayIcon(true);  // Explorer khởi động lại: thêm lại icon
        return 0;
    }
    switch (msg) {
        case WM_TIMER:
            CheckAndInject();
            CheckStatusAndNotify();
            UpdateTrayIcon(false);
            MaybeAutoCheckForUpdate();
            return 0;
        case WM_INJECT_DONE:
            OnInjectDone((DWORD)wParam, (HRESULT)lParam);
            return 0;
        case WM_UPDATE_CHECKED:
            OnUpdateChecked(reinterpret_cast<upd::CheckResult*>(lParam));
            return 0;
        case WM_UPDATE_APPLIED:
            OnUpdateApplied((DWORD)wParam);
            return 0;
        case WM_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_BALLOONUSERCLICK:
                    if (g_balloonIsUpdate) {
                        g_balloonIsUpdate = false;
                        PromptUpdate();
                    }
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    ShowMenu();
                    break;
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    OpenSettingsWindow();  // mở lần 2 chỉ đưa cửa sổ lên trước
                    break;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_SECURE_DATADIR && lParam == 1) {
                PromptSecureDataDir(true);  // ngay sau khi cập nhật OTA
                return 0;
            }
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_DESTROY:
            KillTimer(hWnd, kTimerId);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

////////////////////////////////////////////////////////////////////////////////
// Khởi động
////////////////////////////////////////////////////////////////////////////////

bool HasArg(PCWSTR arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && argv; i++) {
        if (_wcsicmp(argv[i], arg) == 0) {
            found = true;
        }
    }
    LocalFree(argv);
    return found;
}

// Thêm quyền `rights` (kế thừa xuống thư mục con / file) cho các SID dạng chuỗi.
// Chỉ dùng cho thư mục riêng của người dùng (LocalState của gói MSIX).
void GrantAccess(const std::wstring& dir,
                 std::initializer_list<PCWSTR> sidStrings,
                 DWORD rights) {
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &oldDacl, nullptr, &sd) != ERROR_SUCCESS) {
        return;
    }
    std::vector<PSID> sids;
    std::vector<EXPLICIT_ACCESSW> ea;
    for (PCWSTR s : sidStrings) {
        PSID sid = nullptr;
        if (ConvertStringSidToSidW(s, &sid)) {
            sids.push_back(sid);
        }
    }
    for (PSID sid : sids) {
        EXPLICIT_ACCESSW e{};
        e.grfAccessPermissions = rights;
        e.grfAccessMode = GRANT_ACCESS;
        e.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        e.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        e.Trustee.ptstrName = (LPWSTR)sid;
        ea.push_back(e);
    }
    PACL newDacl = nullptr;
    if (!ea.empty() && SetEntriesInAclW((ULONG)ea.size(), ea.data(), oldDacl,
                                        &newDacl) == ERROR_SUCCESS) {
        DWORD rc = SetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, newDacl, nullptr);
        // ACCESS_DENIED: thư mục do bộ cài (Admin) tạo, đã có sẵn quyền.
        if (rc != ERROR_SUCCESS && rc != ERROR_ACCESS_DENIED) {
            LogLine(L"Không cấp được quyền cho " + dir + L": " +
                    upd::Win32ErrorText(rc));
        }
        LocalFree(newDacl);
    }
    for (PSID sid : sids) {
        if (sid) {
            LocalFree(sid);
        }
    }
    LocalFree(sd);
}

// Bản MSIX không chạy install.ps1: tự tạo thư mục dữ liệu <LocalState>\data
// (riêng người dùng này), cho tiến trình bảng lịch (AppContainer) đọc / ghi
// file trong đó, và tạo settings.ini mặc định.
void EnsurePackagedDataDir() {
    std::wstring ls = PackageLocalState();
    if (ls.empty()) {
        return;
    }
    std::wstring dir = DataDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\status").c_str(), nullptr);
    GrantAccess(dir, {L"S-1-15-2-1"},
                FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE);

    std::wstring ini = dir + L"\\" AMLICH_SETTINGS_FILE;
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    std::string bytes =
        datadir::DefaultIniBytes(ExeDir() + L"\\settings.default.ini");
    std::wstring err;
    if (upd::WriteWholeFile(ini, bytes, &err)) {
        LogLine(L"Đã tạo settings.ini mặc định");
    }
}

// Bản MSIX: file trong C:\Program Files\WindowsApps chỉ cho chính gói đọc, nên
// tiến trình bảng lịch (AppContainer khác) không nạp được AmLichTap.dll từ đó.
// Chép DLL sang LocalState của gói (chỉ người dùng hiện tại ghi được) và cho
// mọi AppContainer quyền CHỈ ĐỌC. Không dùng %ProgramData%\AmLichVN vì thư mục
// đó ai cũng ghi được: người dùng khác có thể thay DLL để chạy code trong shell
// của bạn.
std::wstring PrepareTapDll() {
    std::wstring src = ExeDir() + L"\\" AMLICH_TAP_DLL;
    std::wstring ls = PackageLocalState();
    if (ls.empty()) {
        return src;
    }
    std::wstring dir = ls + L"\\tap";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    GrantAccess(dir, {L"S-1-15-2-1"}, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);

    std::wstring name = L"AmLichTap-" AMLICH_VERSION L".dll";
    std::wstring dst = dir + L"\\" + name;
    // Thất bại khi bản cùng phiên bản đang được shell nạp (file bị khóa): dùng
    // luôn bản đó.
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE) &&
        GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine(L"Không chép được AmLichTap.dll: " +
                upd::Win32ErrorText(GetLastError()));
        return src;
    }

    // Dọn DLL của các phiên bản cũ (bản đang bị shell giữ sẽ được xóa ở lần sau).
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\AmLichTap-*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (_wcsicmp(fd.cFileName, name.c_str()) != 0) {
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return dst;
}

bool GetArgValue(PCWSTR arg, std::wstring* value) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i + 1 < argc && argv; i++) {
        if (_wcsicmp(argv[i], arg) == 0) {
            *value = argv[i + 1];
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInst = hInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --- Các lệnh chạy với quyền Administrator ---------------------------
    // Xử lý trước mọi thứ khác và KHÔNG ghi log / đụng tới đường dẫn của người
    // dùng: kết quả trả về qua mã thoát, tiến trình gọi tự ghi log.
    std::wstring applyDir;
    if (GetArgValue(L"--apply-update", &applyDir)) {
        if (IsPackaged()) {
            return upd::kApplyBadArgs;  // gói MSIX không tự ghi đè được
        }
        std::wstring result;
        DWORD code = upd::ApplyUpdate(applyDir, ExeDir(), &result);
        if (code == upd::kApplyOk) {
            // Bản mới được cài: đồng thời siết quyền thư mục dữ liệu.
            datadir::Secure(ExeDir() + L"\\settings.default.ini");
        }
        return (int)code;
    }
    if (HasArg(L"--secure-datadir")) {
        return IsPackaged() ? 1
                            : (int)datadir::Secure(ExeDir() +
                                                   L"\\settings.default.ini");
    }
    if (HasArg(L"--remove-datadir")) {
        return IsPackaged() ? 1 : (int)datadir::Remove();
    }

    g_tapDllPath =
        IsPackaged() ? PrepareTapDll() : ExeDir() + L"\\" AMLICH_TAP_DLL;

    if (HasArg(L"--check-update")) {
        upd::CheckResult r = upd::CheckForUpdate(UpdateRepo());
        std::wstring latest = upd::Utf8ToWide(r.manifest.version);
        switch (r.kind) {
            case upd::CheckResult::Error:
                LogLine(L"--check-update: lỗi: " + r.error);
                return 1;
            case upd::CheckResult::UpToDate:
                LogLine(L"--check-update: đang dùng bản mới nhất, bản phát hành "
                        L"mới nhất " + latest + L" (chữ ký hợp lệ)");
                return 0;
            default:
                LogLine(L"--check-update: có bản mới " + latest +
                        L" (chữ ký hợp lệ)");
                return 10;
        }
    }

    bool openSettings = HasArg(L"--update-settings");
    if (openSettings) {
        if (HWND h = FindWindowW(kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(h, WM_COMMAND, ID_UPDATE_SETTINGS, 0);
            return 0;
        }
    }

    if (HasArg(L"--quit")) {
        if (HWND h = FindWindowW(kWindowClass, nullptr)) {
            PostMessageW(h, WM_COMMAND, ID_EXIT, 0);
        }
        return 0;
    }

    if (HasArg(L"--restart-shell")) {
        RestartShellProcesses();
        return 0;
    }

    if (HasArg(L"--inject-once")) {
        int failures = 0;
        for (const auto& t : FindTargets()) {
            HRESULT hr = InjectInto(t.pid);
            LogLine(L"--inject-once " + t.name + L": " + HrToString(hr));
            if (FAILED(hr)) {
                failures++;
            }
        }
        return failures;
    }

    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\AmLichVN_Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;  // đã có một bản đang chạy
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (IsPackaged()) {
        EnsurePackagedDataDir();
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);
    upd::DeleteTree(upd::LocalUpdateRoot());  // dọn file tải tạm của lần trước
    bool justUpdated = HasArg(L"--updated");
    datadir::State dataState = datadir::State::Secure;
    if (!IsPackaged()) {
        dataState = datadir::Check();
        g_dataDirInsecure = dataState != datadir::State::Secure;
    }
    if (justUpdated) {
        LogLine(L"Đã cập nhật lên phiên bản " AMLICH_VERSION);
        RegSetString(L"UpdateLastResult",
                     L"Đã cập nhật lên phiên bản " AMLICH_VERSION L".");
    }

    // Lần chạy đầu tiên: bật "Khởi động cùng Windows" (tắt được trong menu).
    if (RegGetString(L"AutostartInitialized").empty()) {
        SetAutostart(true);
        RegSetString(L"AutostartInitialized", L"1");
    }

    g_osBuild = ReadOsBuild();
    std::wstring lastSeen = RegGetString(L"LastSeenBuild");
    bool buildChanged = !lastSeen.empty() && lastSeen != g_osBuild;
    RegSetString(L"LastSeenBuild", g_osBuild);
    LogLine(L"Khởi động AmLichVN " AMLICH_VERSION L", Windows build " +
            g_osBuild + (buildChanged ? L" (vừa cập nhật từ " + lastSeen + L")"
                                      : L""));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_hWnd = CreateWindowExW(0, kWindowClass, kAppName, 0, 0, 0, 0, 0,
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd) {
        return 1;
    }

    UpdateTrayIcon(true);

    if (buildChanged) {
        ShowBalloon(L"Windows vừa cập nhật",
                    L"Build " + lastSeen + L" → " + g_osBuild +
                        L". Hãy bấm vào đồng hồ mở lịch một lần để kiểm tra âm "
                        L"lịch còn hoạt động.");
    }

    if (justUpdated) {
        ShowBalloon(L"Đã cập nhật",
                    L"Âm lịch Việt Nam đã được cập nhật lên phiên bản " AMLICH_VERSION
                    L".");
    }

    if (dataState == datadir::State::Insecure) {
        LogLine(L"Thư mục dữ liệu " + DataDir() +
                L" chưa được bảo vệ (bản cài cũ)");
        if (justUpdated) {
            // Vừa cập nhật từ bản cũ qua OTA: hỏi luôn (một lần UAC).
            PostMessageW(g_hWnd, WM_COMMAND, ID_SECURE_DATADIR, 1);
        } else if (!buildChanged) {
            ShowBalloon(L"Cần sửa quyền thư mục dữ liệu",
                        L"Bấm chuột phải vào icon > \"Sửa quyền thư mục dữ "
                        L"liệu\" (cần quyền Admin).",
                        NIIF_WARNING);
        }
    }

    CheckAndInject();
    SetTimer(g_hWnd, kTimerId, kTimerMs, nullptr);
    g_upd.nextAutoCheck = GetTickCount64() + kAutoCheckFirstDelay;
    if (openSettings) {
        OpenSettingsWindow();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon) {
        DestroyIcon(g_icon);
    }
    if (g_mutex) {
        CloseHandle(g_mutex);
    }
    return 0;
}
