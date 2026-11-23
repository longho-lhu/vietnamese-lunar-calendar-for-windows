// AmLichTap.dll - chèn âm lịch Việt Nam vào bảng lịch (đồng hồ taskbar) của
// Windows 11.
//
// DLL này được AmLichVN.exe nạp vào tiến trình giao diện của Windows
// (ShellHost.exe từ Windows 11 24H2, ShellExperienceHost.exe ở bản cũ hơn)
// thông qua InitializeXamlDiagnosticsEx - cơ chế chẩn đoán XAML chính thức mà
// Visual Studio dùng cho "Live Visual Tree". Windows tạo đối tượng COM "TAP"
// trong DLL này, DLL đăng ký nhận sự kiện mỗi khi có phần tử giao diện được
// thêm vào, và khi thấy ô ngày của CalendarView thì gắn thêm dòng âm lịch.
//
// Mã nguồn phát hành theo GNU GPL v3.0. Phần TAP/VisualTreeWatcher dựa trên
// TranslucentTB (GPL v3.0) và mod "Windows 11 Notification Center Styler" của
// m417z (GPL v3.0). Thuật toán âm lịch: Hồ Ngọc Đức.

#include <windows.h>

#include <xamlom.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <Unknwn.h>
#include <ocidl.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include "../common/shared.h"
#include "../common/vnlunar.h"

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

////////////////////////////////////////////////////////////////////////////////
// Đường dẫn, log
////////////////////////////////////////////////////////////////////////////////

std::wstring g_dataDir;      // %ProgramData%\AmLichVN (MSIX: <LocalState>\data)
std::wstring g_userSid;      // SID người dùng, dùng trong tên file trạng thái/log
std::wstring g_processName;  // ShellHost.exe / ShellExperienceHost.exe
HMODULE g_hModule;           // chính DLL này
std::atomic<bool> g_debugLog;
std::mutex g_logMutex;

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr,
                                0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::wstring NowString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR buf[64];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Ghi đè hoặc nối thêm một file UTF-8 trong <dữ liệu>\status.
// FILE_FLAG_OPEN_REPARSE_POINT: nếu ai đó đặt symlink trùng tên thì mở chính
// symlink chứ không đi theo nó tới file khác (và bị loại ở bước kiểm tra).
void WriteUtf8File(const std::wstring& path,
                   const std::wstring& text,
                   bool append) {
    // Không cắt ngắn khi mở (OPEN_ALWAYS); chỉ ghi khi đó là file thường, một
    // tên, do chính người dùng này sở hữu. File "chiếm chỗ" của người khác, hard
    // link hay reparse point đều bị bỏ qua.
    HANDLE file = CreateFileW(
        path.c_str(),
        (append ? FILE_APPEND_DATA : (FILE_WRITE_DATA | FILE_APPEND_DATA)) |
            FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!amlich::IsOwnSingleLinkFile(file)) {
        CloseHandle(file);
        return;
    }
    if (append) {
        LARGE_INTEGER size{};
        // Giữ log nhỏ: quá 512 KB thì bỏ qua (người dùng xóa được file của mình).
        if (GetFileSizeEx(file, &size) && size.QuadPart > 512 * 1024) {
            CloseHandle(file);
            return;
        }
    } else {
        SetEndOfFile(file);  // con trỏ đang ở đầu file: cắt về 0
    }
    std::string utf8 = ToUtf8(text);
    DWORD written;
    WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(file);
}

void LogLine(PCWSTR fmt, ...) {
    WCHAR buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);

    std::wstring line = L"[AmLichVN] ";
    line += buf;
    line += L"\r\n";
    OutputDebugStringW(line.c_str());

    if (g_debugLog && !g_dataDir.empty()) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        WriteUtf8File(amlich::DllLogFilePath(g_dataDir, g_userSid, g_processName),
                      NowString() + L" " + line, true);
    }
}

#define LOG(fmt, ...) LogLine(fmt, ##__VA_ARGS__)

////////////////////////////////////////////////////////////////////////////////
// Cài đặt (settings.ini) - tự đọc lại khi file thay đổi
////////////////////////////////////////////////////////////////////////////////

struct Settings {
    bool dayMonth = false;
    double fontSize = 10;
    double opacity = 0.8;
    double bottomMargin = 1;
    bool moveDayNumberUp = true;
    double dayNumberFontSize = 0;
    bool highlightSpecialDays = true;
    winrt::Windows::UI::Color highlightColor{255, 0xE0, 0x40, 0x40};
    bool showSolarTerm = false;
    bool showTooltip = true;
    bool showDayInfo = true;
};

Settings g_settings;
std::mutex g_settingsMutex;
FILETIME g_settingsWriteTime{};
ULONGLONG g_settingsLastCheck = 0;

Settings GetSettings() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

bool ParseHexColor(const std::wstring& str, winrt::Windows::UI::Color* out) {
    std::wstring s = str;
    s.erase(std::remove(s.begin(), s.end(), L' '), s.end());
    if (!s.empty() && s[0] == L'#') {
        s.erase(0, 1);
    }
    if (s.size() != 6 && s.size() != 8) {
        return false;
    }
    unsigned long v = 0;
    for (wchar_t c : s) {
        int d;
        if (c >= L'0' && c <= L'9') {
            d = c - L'0';
        } else if (c >= L'a' && c <= L'f') {
            d = c - L'a' + 10;
        } else if (c >= L'A' && c <= L'F') {
            d = c - L'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | (unsigned long)d;
    }
    if (s.size() == 6) {
        v |= 0xFF000000;
    }
    out->A = (uint8_t)(v >> 24);
    out->R = (uint8_t)(v >> 16);
    out->G = (uint8_t)(v >> 8);
    out->B = (uint8_t)v;
    return true;
}

std::wstring SettingsPath() {
    return g_dataDir + L"\\" AMLICH_SETTINGS_FILE;
}

void LoadSettings() {
    std::wstring path = SettingsPath();
    auto readInt = [&](PCWSTR key, int def) {
        return (int)GetPrivateProfileIntW(AMLICH_INI_SECTION, key, def,
                                          path.c_str());
    };
    auto readStr = [&](PCWSTR key, PCWSTR def) {
        WCHAR buf[128];
        GetPrivateProfileStringW(AMLICH_INI_SECTION, key, def, buf,
                                 ARRAYSIZE(buf), path.c_str());
        return std::wstring(buf);
    };

    Settings s;
    s.dayMonth = readStr(L"format", L"dayOnly") == L"dayMonth";
    int v = readInt(L"fontSize", 10);
    s.fontSize = (v >= 6 && v <= 30) ? v : 10;
    v = readInt(L"opacity", 80);
    s.opacity = (v >= 10 && v <= 100) ? v / 100.0 : 0.8;
    v = readInt(L"bottomMargin", 1);
    s.bottomMargin = (v >= 0 && v <= 30) ? v : 1;
    s.moveDayNumberUp = readInt(L"moveDayNumberUp", 1) != 0;
    v = readInt(L"dayNumberFontSize", 0);
    s.dayNumberFontSize = (v >= 6 && v <= 40) ? v : 0;
    s.highlightSpecialDays = readInt(L"highlightSpecialDays", 1) != 0;
    winrt::Windows::UI::Color c;
    if (ParseHexColor(readStr(L"highlightColor", L"#E04040"), &c)) {
        s.highlightColor = c;
    }
    s.showSolarTerm = readInt(L"showSolarTerm", 0) != 0;
    s.showTooltip = readInt(L"showTooltip", 1) != 0;
    s.showDayInfo = readInt(L"showDayInfo", 1) != 0;
    g_debugLog = readInt(L"debugLog", 0) != 0;

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = s;
}

// Trả về true nếu settings.ini vừa thay đổi (và đã đọc lại).
std::mutex g_reloadMutex;

bool ReloadSettingsIfChanged() {
    std::lock_guard<std::mutex> reloadLock(g_reloadMutex);
    ULONGLONG now = GetTickCount64();
    if (now - g_settingsLastCheck < 1500) {
        return false;
    }
    g_settingsLastCheck = now;

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard,
                              &data)) {
        return false;
    }
    if (CompareFileTime(&data.ftLastWriteTime, &g_settingsWriteTime) == 0) {
        return false;
    }
    g_settingsWriteTime = data.ftLastWriteTime;
    LoadSettings();
    LOG(L"Đã đọc lại settings.ini");
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Trạng thái tự kiểm tra -> <dữ liệu>\status\status-<SID>-<tiến trình>.txt
// AmLichVN.exe của cùng người dùng đọc file này để biết có hoạt động hay không.
////////////////////////////////////////////////////////////////////////////////

struct Status {
    std::wstring osBuild;
    std::wstring state = L"PENDING";  // PENDING / OK / FAIL
    std::wstring detail = L"Đã nạp, chưa thấy bảng lịch (hãy mở lịch)";
    int calendarViews = 0;
    int dayItems = 0;
    int errors = 0;
    std::wstring lastError;
    std::vector<std::wstring> calendarLikeTypes;
};

Status g_status;
std::mutex g_statusMutex;

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

// Gọi khi đang giữ g_statusMutex.
void WriteStatusFileLocked() {
    if (g_dataDir.empty()) {
        return;
    }
    std::wstring types;
    for (const auto& t : g_status.calendarLikeTypes) {
        if (!types.empty()) {
            types += L"; ";
        }
        types += t;
    }

    std::wstring text;
    text += L"version=" AMLICH_VERSION L"\r\n";
    text += L"build=" + g_status.osBuild + L"\r\n";
    text += L"process=" + g_processName + L"\r\n";
    WCHAR dllPath[MAX_PATH] = L"";
    GetModuleFileNameW(g_hModule, dllPath, ARRAYSIZE(dllPath));
    text += L"dll=" + std::wstring(dllPath) + L"\r\n";
    text += L"pid=" + std::to_wstring(GetCurrentProcessId()) + L"\r\n";
    text += L"state=" + g_status.state + L"\r\n";
    text += L"detail=" + g_status.detail + L"\r\n";
    text += L"time=" + NowString() + L"\r\n";
    text += L"calendarViews=" + std::to_wstring(g_status.calendarViews) +
            L"\r\n";
    text += L"dayItems=" + std::to_wstring(g_status.dayItems) + L"\r\n";
    text += L"errors=" + std::to_wstring(g_status.errors) + L"\r\n";
    text += L"lastError=" + g_status.lastError + L"\r\n";
    text += L"calendarLikeTypes=" + types + L"\r\n";

    WriteUtf8File(amlich::StatusFilePath(g_dataDir, g_userSid, g_processName),
                  text, false);
}

void RecordError(PCWSTR where, HRESULT hr) {
    WCHAR buf[256];
    swprintf_s(buf, L"%ls: 0x%08X", where, (unsigned)hr);
    LOG(L"Lỗi %ls", buf);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.errors++;
    g_status.lastError = buf;
    WriteStatusFileLocked();
}

void SetState(PCWSTR state, const std::wstring& detail) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.state = state;
        g_status.detail = detail;
        WriteStatusFileLocked();
    }
    LOG(L"Trạng thái: %ls - %ls", state, detail.c_str());
}

////////////////////////////////////////////////////////////////////////////////
// TAP + VisualTreeWatcher
////////////////////////////////////////////////////////////////////////////////

// XamlDiagnostics cũng hiện thực interface này (không có trong xamlom.h).
// UnregisterInstance nhả tham chiếu mà bộ chẩn đoán giữ cho mỗi phần tử đã báo,
// để không rò rỉ bộ nhớ của shell.
static constexpr GUID IID_IXamlDiagnosticsTestHooks = {
    0x735941a2,
    0x3ee3,
    0x495a,
    {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE
    UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    TryGetDispatcherQueueForObject(InstanceHandle handle,
                                   void** dispatcherQueue) = 0;
};

void OnElementAdded(wf::IInspectable const& element,
                    PCWSTR type,
                    wf::IInspectable const& parent);
void QueueDiagnosticsRelease(InstanceHandle handle);
void FlushDiagnosticsReleasesIfQuiet();

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher,
                               IVisualTreeServiceCallback2,
                               winrt::non_agile> {
   public:
    VisualTreeWatcher(winrt::com_ptr<IUnknown> site);

    void UnadviseVisualTreeChange() {
        HRESULT hr = m_XamlDiagnostics.as<IVisualTreeService3>()
                         ->UnadviseVisualTreeChange(this);
        if (FAILED(hr)) {
            LOG(L"UnadviseVisualTreeChange lỗi 0x%08X", (unsigned)hr);
        }
    }

    void ReleaseDiagnosticsReference(InstanceHandle handle) {
        if (m_XamlDiagnosticsTestHooks) {
            m_XamlDiagnosticsTestHooks->UnregisterInstance(handle);
        }
    }

   private:
    HRESULT STDMETHODCALLTYPE
    OnVisualTreeChange(ParentChildRelation relation,
                       VisualElement element,
                       VisualMutationType mutationType) override;

    HRESULT STDMETHODCALLTYPE
    OnElementStateChanged(InstanceHandle,
                          VisualElementState,
                          LPCWSTR) noexcept override {
        return S_OK;
    }

    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable obj;
        winrt::check_hresult(m_XamlDiagnostics->GetIInspectableFromHandle(
            handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
        return obj;
    }

    winrt::com_ptr<IXamlDiagnostics> m_XamlDiagnostics = nullptr;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> m_XamlDiagnosticsTestHooks =
        nullptr;
};

// Cố ý không bao giờ hủy (tránh nhả COM object lúc tiến trình đang thoát).
winrt::com_ptr<VisualTreeWatcher>& WatcherSlot() {
    static auto* slot = new winrt::com_ptr<VisualTreeWatcher>();
    return *slot;
}
SRWLOCK g_visualTreeWatcherLock = SRWLOCK_INIT;

winrt::com_ptr<VisualTreeWatcher> GetVisualTreeWatcher() {
    AcquireSRWLockShared(&g_visualTreeWatcherLock);
    auto watcher = WatcherSlot();
    ReleaseSRWLockShared(&g_visualTreeWatcherLock);
    return watcher;
}

winrt::com_ptr<VisualTreeWatcher> ExchangeVisualTreeWatcher(
    winrt::com_ptr<VisualTreeWatcher> watcher) {
    AcquireSRWLockExclusive(&g_visualTreeWatcherLock);
    std::swap(WatcherSlot(), watcher);
    ReleaseSRWLockExclusive(&g_visualTreeWatcherLock);
    return watcher;
}

VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
    : m_XamlDiagnostics(site.as<IXamlDiagnostics>()) {
    LOG(L"Tạo VisualTreeWatcher");

    HRESULT hr = m_XamlDiagnostics->QueryInterface(
        IID_IXamlDiagnosticsTestHooks, m_XamlDiagnosticsTestHooks.put_void());
    if (FAILED(hr)) {
        LOG(L"Không có IXamlDiagnosticsTestHooks (0x%08X)", (unsigned)hr);
    }

    // Gọi AdviseVisualTreeChange ngay trên luồng này đôi khi làm treo, nên
    // gọi từ một luồng mới.
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID lpParam) -> DWORD {
            auto watcher = reinterpret_cast<VisualTreeWatcher*>(lpParam);
            HRESULT hr = watcher->m_XamlDiagnostics.as<IVisualTreeService3>()
                             ->AdviseVisualTreeChange(watcher);
            watcher->Release();
            if (FAILED(hr)) {
                RecordError(L"AdviseVisualTreeChange", hr);
                SetState(L"FAIL",
                         L"Không đăng ký được theo dõi giao diện "
                         L"(AdviseVisualTreeChange)");
            } else {
                LOG(L"Đã đăng ký theo dõi cây giao diện");
            }
            return 0;
        },
        this, 0, nullptr);
    if (thread) {
        AddRef();
        CloseHandle(thread);
    }
}

HRESULT VisualTreeWatcher::OnVisualTreeChange(
    ParentChildRelation relation,
    VisualElement element,
    VisualMutationType mutationType) try {
    if (mutationType == Add && element.Type) {
        try {
            PCWSTR type = element.Type;
            bool interesting =
                wcsstr(type, L"Calendar") != nullptr ||
                wcscmp(type, L"Windows.UI.Xaml.Controls.TextBlock") == 0;
            if (interesting) {
                wf::IInspectable inspectable = FromHandle(element.Handle);
                wf::IInspectable parent{nullptr};
                if (relation.Parent) {
                    if (FAILED(m_XamlDiagnostics->GetIInspectableFromHandle(
                            relation.Parent,
                            reinterpret_cast<::IInspectable**>(
                                winrt::put_abi(parent))))) {
                        parent = nullptr;
                    }
                }
                OnElementAdded(inspectable, type, parent);
            }
        } catch (...) {
            RecordError(L"OnVisualTreeChange", winrt::to_hresult());
        }
    }

    FlushDiagnosticsReleasesIfQuiet();

    if (mutationType == Add) {
        QueueDiagnosticsRelease(element.Handle);
        QueueDiagnosticsRelease(relation.Parent);
    } else if (mutationType == Remove) {
        QueueDiagnosticsRelease(element.Handle);
    }
    return S_OK;
} catch (...) {
    // Trả lỗi sẽ làm XAML ngừng gửi sự kiện, nên luôn trả S_OK.
    return S_OK;
}

class AmLichTap
    : public winrt::implements<AmLichTap, IObjectWithSite, winrt::non_agile> {
   public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override try {
        if (auto previous = ExchangeVisualTreeWatcher(nullptr)) {
            previous->UnadviseVisualTreeChange();
        }
        site.copy_from(pUnkSite);
        if (site) {
            ExchangeVisualTreeWatcher(winrt::make_self<VisualTreeWatcher>(site));
        }
        return S_OK;
    } catch (...) {
        HRESULT hr = winrt::to_hresult();
        RecordError(L"TAP SetSite", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid,
                                      void** ppvSite) noexcept override {
        return site.as(riid, ppvSite);
    }

   private:
    winrt::com_ptr<IUnknown> site;
};

template <class T>
struct SimpleFactory
    : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter,
                                             REFIID riid,
                                             void** ppvObject) override try {
        *ppvObject = nullptr;
        if (pUnkOuter) {
            return CLASS_E_NOAGGREGATION;
        }
        return winrt::make<T>().as(riid, ppvObject);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

void EnsureInitialized();

// Xuất qua AmLichTap.def
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    *ppv = nullptr;
    if (rclsid == CLSID_AmLichTap) {
        EnsureInitialized();
        return winrt::make<SimpleFactory<AmLichTap>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return winrt::to_hresult();
}

// DLL đã gắn callback vào giao diện shell nên không bao giờ được gỡ khỏi bộ
// nhớ khi tiến trình còn chạy. Muốn gỡ hẳn: khởi động lại tiến trình shell.
STDAPI DllCanUnloadNow() {
    return S_FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// Trạng thái theo từng luồng UI
////////////////////////////////////////////////////////////////////////////////

struct TrackedItem {
    winrt::weak_ref<wuxc::CalendarViewDayItem> item;
    int64_t dateToken = 0;
    wf::IInspectable originalToolTip{nullptr};
    bool toolTipSet = false;
    bool counted = false;
};

struct TrackedCalendar {
    winrt::weak_ref<wuxc::CalendarView> view;
    winrt::event_token dayItemChangingToken{};
    winrt::event_token selectedDatesToken{};
    winrt::weak_ref<wuxc::StackPanel> infoPanel;
    int infoDay = 0;  // ngày (dương) lúc cập nhật khung thông tin gần nhất
    wf::IInspectable originalVAlign{nullptr};
    wf::IInspectable originalDayFontSize{nullptr};
    bool vAlignSet = false;
    bool fontSizeSet = false;
};

constexpr ULONGLONG kDiagnosticsReleaseDelay = 200;

// Dùng shared_ptr vì các thao tác XAML (đổi template, ApplyTemplate...) có thể
// gọi ngược lại vào DLL ngay lập tức (re-entrancy) và làm thay đổi danh sách.
struct ThreadState {
    winrt::Windows::System::DispatcherQueue dispatcherQueue{nullptr};
    wuxc::ControlTemplate lunarTemplate{nullptr};
    std::vector<std::shared_ptr<TrackedItem>> items;
    std::vector<std::shared_ptr<TrackedCalendar>> calendars;

    std::vector<InstanceHandle> pendingRelease;
    ULONGLONG lastReleaseQueueTick = 0;
    bool releaseDrainQueued = false;
    winrt::Windows::System::DispatcherQueueTimer releaseTimer{nullptr};

    winrt::Windows::System::DispatcherQueueTimer selfTestTimer{nullptr};
};

thread_local ThreadState* t_state;

ThreadState& GetThreadState() {
    if (!t_state) {
        t_state = new ThreadState();
        try {
            t_state->dispatcherQueue =
                winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        } catch (...) {
        }
    }
    return *t_state;
}

void QueueDiagnosticsRelease(InstanceHandle handle) {
    if (!handle) {
        return;
    }
    auto& st = GetThreadState();
    st.pendingRelease.push_back(handle);
    st.lastReleaseQueueTick = GetTickCount64();
}

void FlushDiagnosticsReleases() {
    if (!t_state) {
        return;
    }
    auto pending = std::move(t_state->pendingRelease);
    t_state->pendingRelease.clear();
    auto watcher = GetVisualTreeWatcher();
    if (!watcher) {
        return;
    }
    std::sort(pending.begin(), pending.end());
    pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
    for (InstanceHandle h : pending) {
        watcher->ReleaseDiagnosticsReference(h);
    }
}

// Không được nhả tham chiếu ngay khi XAML đang duyệt cây, nên chờ đợt cập nhật
// kết thúc rồi nhả bằng một timer 1 lần.
void FlushDiagnosticsReleasesIfQuiet() {
    auto& st = GetThreadState();
    if (st.pendingRelease.empty() || st.releaseDrainQueued ||
        GetTickCount64() - st.lastReleaseQueueTick < kDiagnosticsReleaseDelay) {
        return;
    }
    try {
        if (!st.releaseTimer) {
            if (!st.dispatcherQueue) {
                return;
            }
            st.releaseTimer = st.dispatcherQueue.CreateTimer();
            st.releaseTimer.IsRepeating(false);
            st.releaseTimer.Interval(std::chrono::milliseconds{1});
            st.releaseTimer.Tick(
                [](winrt::Windows::System::DispatcherQueueTimer const&,
                   wf::IInspectable const&) {
                    if (!t_state) {
                        return;
                    }
                    t_state->releaseDrainQueued = false;
                    FlushDiagnosticsReleases();
                });
        }
        st.releaseTimer.Start();
        st.releaseDrainQueued = true;
    } catch (...) {
        RecordError(L"ReleaseTimer", winrt::to_hresult());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Gắn âm lịch vào từng ô ngày
////////////////////////////////////////////////////////////////////////////////

constexpr wchar_t kLunarRootName[] = L"AmLichVnRoot";

// Theo tài liệu của Microsoft, template mặc định của CalendarViewDayItem là
// rỗng và được phép chèn phần tử XAML tùy ý; phần tử chèn vào nằm trên nền và
// dưới số ngày.
wuxc::ControlTemplate GetLunarTemplate() {
    auto& st = GetThreadState();
    if (!st.lunarTemplate) {
        st.lunarTemplate =
            wux::Markup::XamlReader::Load(
                LR"(<ControlTemplate
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    TargetType="CalendarViewDayItem">
  <Grid x:Name="AmLichVnRoot" IsHitTestVisible="False">
    <TextBlock HorizontalAlignment="Center"
               VerticalAlignment="Bottom"
               TextAlignment="Center"
               TextWrapping="NoWrap"
               TextTrimming="CharacterEllipsis"
               IsTextScaleFactorEnabled="False" />
  </Grid>
</ControlTemplate>)")
                .as<wuxc::ControlTemplate>();
    }
    return st.lunarTemplate;
}

wuxc::TextBlock FindLunarTextBlock(wuxc::CalendarViewDayItem const& item) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(item);
    for (int i = 0; i < count; i++) {
        auto fe = wuxm::VisualTreeHelper::GetChild(item, i)
                      .try_as<wux::FrameworkElement>();
        if (fe && fe.Name() == kLunarRootName &&
            wuxm::VisualTreeHelper::GetChildrenCount(fe) > 0) {
            return wuxm::VisualTreeHelper::GetChild(fe, 0)
                .try_as<wuxc::TextBlock>();
        }
    }
    return nullptr;
}

// TextBlock số ngày dương do CalendarView tự thêm (con trực tiếp của ô ngày).
wuxc::TextBlock FindDayNumberTextBlock(wuxc::CalendarViewDayItem const& item) {
    int count = wuxm::VisualTreeHelper::GetChildrenCount(item);
    for (int i = 0; i < count; i++) {
        if (auto tb = wuxm::VisualTreeHelper::GetChild(item, i)
                          .try_as<wuxc::TextBlock>()) {
            return tb;
        }
    }
    return nullptr;
}

struct LocalDate {
    int day, month, year;
};

LocalDate ToLocalDate(wf::DateTime dt) {
    winrt::file_time ft = winrt::clock::to_file_time(dt);
    FILETIME utc;
    utc.dwLowDateTime = (DWORD)(ft.value & 0xFFFFFFFF);
    utc.dwHighDateTime = (DWORD)(ft.value >> 32);
    SYSTEMTIME stUtc, stLocal;
    FileTimeToSystemTime(&utc, &stUtc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &stUtc, &stLocal)) {
        stLocal = stUtc;
    }
    return {stLocal.wDay, stLocal.wMonth, stLocal.wYear};
}

bool IsToday(const LocalDate& d) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wDay == d.day && st.wMonth == d.month && st.wYear == d.year;
}

std::shared_ptr<TrackedItem> FindTrackedItem(
    wuxc::CalendarViewDayItem const& item) {
    if (!t_state) {
        return nullptr;
    }
    auto& items = t_state->items;
    std::shared_ptr<TrackedItem> found;
    for (size_t i = 0; i < items.size();) {
        auto alive = items[i]->item.get();
        if (!alive) {  // ô đã bị hủy
            items.erase(items.begin() + i);
            continue;
        }
        if (alive == item) {
            found = items[i];
        }
        i++;
    }
    return found;
}

void RestoreLocal(wux::DependencyObject const& obj,
                  wux::DependencyProperty const& dp,
                  wf::IInspectable const& original) {
    if (!original || original == wux::DependencyProperty::UnsetValue()) {
        obj.ClearValue(dp);
    } else {
        obj.SetValue(dp, original);
    }
}

void UpdateDayItem(wuxc::CalendarViewDayItem const& item) {
    auto tracked = FindTrackedItem(item);
    if (!tracked) {
        return;
    }

    auto tb = FindLunarTextBlock(item);
    if (!tb) {
        item.ApplyTemplate();
        tb = FindLunarTextBlock(item);
    }
    if (!tb) {
        // Template chưa dựng xong; sẽ cập nhật lại khi TextBlock số ngày được
        // thêm vào hoặc khi ngày của ô thay đổi.
        return;
    }

    const Settings s = GetSettings();
    LocalDate d = ToLocalDate(item.Date());
    vnlunar::LunarDate l = vnlunar::SolarToLunar(d.day, d.month, d.year);

    std::wstring text;
    if (s.showSolarTerm && vnlunar::SolarTermStartsOn(l.jd)) {
        text = vnlunar::kSolarTerms[vnlunar::SolarTermIndex(l.jd)];
    } else if (s.dayMonth || l.day == 1) {
        text = std::to_wstring(l.day) + L"/" + std::to_wstring(l.month);
        if (l.leap) {
            text += L"N";
        }
    } else {
        text = std::to_wstring(l.day);
    }

    tb.Text(text);
    tb.FontSize(s.fontSize);
    tb.Opacity(s.opacity);
    tb.Margin(wux::Thickness{0, 0, 0, s.bottomMargin});

    bool special = s.highlightSpecialDays &&
                   (l.day == 1 || l.day == 15 ||
                    vnlunar::LunarHoliday(l) != nullptr);

    if (special && !IsToday(d)) {
        tb.Foreground(wuxm::SolidColorBrush(s.highlightColor));
        tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    } else {
        tb.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
        // Lấy màu chữ theo số ngày dương, để tự đổi theo trạng thái của ô
        // (hôm nay, ô của tháng khác, ô đang chọn...).
        if (auto dayTb = FindDayNumberTextBlock(item)) {
            wux::Data::Binding binding;
            binding.Source(dayTb);
            binding.Path(wux::PropertyPath(L"Foreground"));
            tb.SetBinding(wuxc::TextBlock::ForegroundProperty(), binding);
        } else {
            tb.ClearValue(wuxc::TextBlock::ForegroundProperty());
        }
    }

    if (s.showTooltip) {
        if (!tracked->toolTipSet) {
            tracked->originalToolTip =
                item.ReadLocalValue(wuxc::ToolTipService::ToolTipProperty());
            tracked->toolTipSet = true;
        }
        wuxc::ToolTipService::SetToolTip(
            item, winrt::box_value(winrt::hstring(amlich::BuildDescription(l))));
    } else if (tracked->toolTipSet) {
        tracked->toolTipSet = false;
        RestoreLocal(item, wuxc::ToolTipService::ToolTipProperty(),
                     tracked->originalToolTip);
    }

    if (!tracked->counted) {
        tracked->counted = true;
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.dayItems++;
    }
}

void ProcessDayItem(wuxc::CalendarViewDayItem const& item) {
    auto& st = GetThreadState();
    auto tmpl = GetLunarTemplate();

    if (!FindTrackedItem(item)) {
        auto t = std::make_shared<TrackedItem>();
        t->item = winrt::make_weak(item);
        t->dateToken = item.RegisterPropertyChangedCallback(
            wuxc::CalendarViewDayItem::DateProperty(),
            [](wux::DependencyObject const& sender,
               wux::DependencyProperty const&) {
                try {
                    if (auto i = sender.try_as<wuxc::CalendarViewDayItem>()) {
                        UpdateDayItem(i);
                    }
                } catch (...) {
                    RecordError(L"DateChanged", winrt::to_hresult());
                }
            });
        st.items.push_back(t);
    }

    if (item.Template() != tmpl) {
        item.Template(tmpl);
    }

    UpdateDayItem(item);
}

void ApplyCalendarSettings(TrackedCalendar& tc, wuxc::CalendarView const& cv) {
    const Settings s = GetSettings();

    if (s.moveDayNumberUp) {
        if (!tc.vAlignSet) {
            tc.originalVAlign = cv.ReadLocalValue(
                wuxc::CalendarView::VerticalDayItemAlignmentProperty());
            tc.vAlignSet = true;
        }
        cv.VerticalDayItemAlignment(wux::VerticalAlignment::Top);
    } else if (tc.vAlignSet) {
        tc.vAlignSet = false;
        RestoreLocal(cv, wuxc::CalendarView::VerticalDayItemAlignmentProperty(),
                     tc.originalVAlign);
    }

    if (s.dayNumberFontSize > 0) {
        if (!tc.fontSizeSet) {
            tc.originalDayFontSize = cv.ReadLocalValue(
                wuxc::CalendarView::DayItemFontSizeProperty());
            tc.fontSizeSet = true;
        }
        cv.DayItemFontSize(s.dayNumberFontSize);
    } else if (tc.fontSizeSet) {
        tc.fontSizeSet = false;
        RestoreLocal(cv, wuxc::CalendarView::DayItemFontSizeProperty(),
                     tc.originalDayFontSize);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Khung thông tin âm lịch của ngày (dưới cùng bảng lịch)
////////////////////////////////////////////////////////////////////////////////

constexpr wchar_t kInfoPanelName[] = L"AmLichVnInfoPanel";

std::wstring WeekdayName(long jd) {
    // Số ngày Julius chia 7 dư 0 là thứ Hai.
    static const wchar_t* const kNames[7] = {L"Thứ Hai", L"Thứ Ba",  L"Thứ Tư",
                                             L"Thứ Năm", L"Thứ Sáu", L"Thứ Bảy",
                                             L"Chủ nhật"};
    return kNames[((jd % 7) + 7) % 7];
}

// Grid chứa bảng lịch của flyout đồng hồ (Windows 11: "CalendarSection", các
// hàng: tiêu đề / đồng hồ / lịch / Focus). Khung thông tin được thêm vào một
// hàng MỚI ở cuối, không đụng tới vị trí các phần tử sẵn có của Windows.
wuxc::Grid FindInfoHost(wuxc::CalendarView const& cv) {
    wux::DependencyObject cur = cv;
    wuxc::Grid fallback{nullptr};
    for (int depth = 0; cur && depth < 10; depth++) {
        cur = wuxm::VisualTreeHelper::GetParent(cur);
        auto fe = cur ? cur.try_as<wux::FrameworkElement>() : nullptr;
        if (!fe) {
            continue;
        }
        if (fe.Name() == L"CalendarSection") {
            if (auto g = fe.try_as<wuxc::Grid>();
                g && g.RowDefinitions().Size() > 0) {
                return g;
            }
        }
        if (!fallback && fe.Name() == L"CalendarControlScrollViewer") {
            auto g = wuxm::VisualTreeHelper::GetParent(fe).try_as<wuxc::Grid>();
            if (g && g.RowDefinitions().Size() > 0) {
                fallback = g;
            }
        }
        if (winrt::get_class_name(cur) == L"ActionCenter.ClockCalendarView") {
            break;
        }
    }
    return fallback;
}

wuxc::StackPanel CreateInfoPanel() {
    return wux::Markup::XamlReader::Load(
               LR"(<StackPanel
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    x:Name="AmLichVnInfoPanel" Margin="0,4,0,12">
  <Border Height="1" Margin="0,0,0,10"
          Background="{ThemeResource SystemControlForegroundBaseLowBrush}" />
  <TextBlock FontSize="12" TextWrapping="Wrap"
             Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
  <TextBlock FontSize="15" FontWeight="SemiBold" Margin="0,2,0,0" TextWrapping="Wrap"
             Foreground="{ThemeResource SystemControlForegroundBaseHighBrush}" />
  <TextBlock FontSize="12" Margin="0,6,0,0" LineHeight="18" TextWrapping="Wrap"
             Foreground="{ThemeResource SystemControlForegroundBaseMediumHighBrush}" />
  <TextBlock FontSize="12" FontWeight="SemiBold" Margin="0,6,0,0"
             TextWrapping="Wrap" Visibility="Collapsed" />
</StackPanel>)")
        .as<wuxc::StackPanel>();
}

void UpdateInfoPanel(wuxc::StackPanel const& panel, wuxc::CalendarView const& cv) {
    const Settings s = GetSettings();
    panel.Visibility(s.showDayInfo ? wux::Visibility::Visible
                                   : wux::Visibility::Collapsed);
    auto children = panel.Children();
    if (!s.showDayInfo || children.Size() < 5) {
        return;
    }

    // Ngày đang chọn trong lịch (nếu có), mặc định là hôm nay.
    SYSTEMTIME now;
    GetLocalTime(&now);
    LocalDate d{now.wDay, now.wMonth, now.wYear};
    auto selected = cv.SelectedDates();
    if (selected && selected.Size() > 0) {
        d = ToLocalDate(selected.GetAt(selected.Size() - 1));
    }
    vnlunar::LunarDate l = vnlunar::SolarToLunar(d.day, d.month, d.year);

    WCHAR solar[32];
    swprintf_s(solar, L"%02d/%02d/%04d", d.day, d.month, d.year);
    std::wstring caption = (IsToday(d) ? L"Hôm nay · " : L"") +
                           WeekdayName(l.jd) + L", " + solar;
    std::wstring title = L"Ngày " + std::to_wstring(l.day) + L" tháng " +
                         amlich::MonthName(l.month) +
                         (l.leap ? L" nhuận" : L"") + L" năm " +
                         vnlunar::YearCanChi(l.year);
    std::wstring details =
        L"Ngày " + vnlunar::DayCanChi(l.jd) + L" · tháng " +
        vnlunar::MonthCanChi(l.month, l.year) +
        (l.monthLength == 30 ? L" (tháng đủ)" : L" (tháng thiếu)") +
        L"\nTiết khí: " + vnlunar::kSolarTerms[vnlunar::SolarTermIndex(l.jd)] +
        (vnlunar::SolarTermStartsOn(l.jd) ? L" (bắt đầu từ ngày này)" : L"") +
        L"\nGiờ hoàng đạo: " + vnlunar::GioHoangDao(l.jd);
    std::wstring special;
    if (const wchar_t* holiday = vnlunar::LunarHoliday(l)) {
        special = holiday;
    } else if (l.day == 1) {
        special = L"Mùng 1 đầu tháng";
    } else if (l.day == 15) {
        special = L"Ngày rằm";
    }

    children.GetAt(1).as<wuxc::TextBlock>().Text(caption);
    children.GetAt(2).as<wuxc::TextBlock>().Text(title);
    children.GetAt(3).as<wuxc::TextBlock>().Text(details);
    auto specialTb = children.GetAt(4).as<wuxc::TextBlock>();
    specialTb.Text(special);
    specialTb.Foreground(wuxm::SolidColorBrush(s.highlightColor));
    specialTb.Visibility(special.empty() ? wux::Visibility::Collapsed
                                         : wux::Visibility::Visible);
}

void EnsureInfoPanel(TrackedCalendar& tc, wuxc::CalendarView const& cv) {
    if (auto panel = tc.infoPanel.get()) {
        UpdateInfoPanel(panel, cv);
        SYSTEMTIME now;
        GetLocalTime(&now);
        tc.infoDay = now.wDay;
        return;
    }
    if (!GetSettings().showDayInfo) {
        return;
    }
    auto host = FindInfoHost(cv);
    if (!host) {
        LOG(L"Chưa tìm thấy chỗ đặt khung thông tin ngày");
        return;
    }

    wuxc::StackPanel panel{nullptr};
    for (auto const& child : host.Children()) {
        auto fe = child.try_as<wux::FrameworkElement>();
        if (fe && fe.Name() == kInfoPanelName) {
            panel = fe.try_as<wuxc::StackPanel>();
        }
    }
    if (!panel) {
        panel = CreateInfoPanel();
        wuxc::RowDefinition row;
        row.Height(wux::GridLengthHelper::Auto());
        host.RowDefinitions().Append(row);
        int index = (int)host.RowDefinitions().Size() - 1;
        wuxc::Grid::SetRow(panel, index);
        if (host.ColumnDefinitions().Size() > 1) {
            wuxc::Grid::SetColumnSpan(panel,
                                      (int)host.ColumnDefinitions().Size());
        }
        // Mỗi lần mở lại bảng lịch: cập nhật (có thể đã sang ngày mới).
        panel.Loaded([weakView = winrt::make_weak(cv)](
                         wf::IInspectable const& sender,
                         wux::RoutedEventArgs const&) {
            try {
                if (auto view = weakView.get()) {
                    UpdateInfoPanel(sender.as<wuxc::StackPanel>(), view);
                }
            } catch (...) {
                RecordError(L"InfoPanelLoaded", winrt::to_hresult());
            }
        });
        host.Children().Append(panel);
        LOG(L"Đã thêm khung thông tin ngày vào hàng %d", index);
    }
    tc.infoPanel = winrt::make_weak(panel);
    UpdateInfoPanel(panel, cv);
    SYSTEMTIME now;
    GetLocalTime(&now);
    tc.infoDay = now.wDay;
}

// Áp dụng lại cài đặt cho mọi thứ trên luồng hiện tại.
void RefreshCurrentThread() {
    if (!t_state) {
        return;
    }
    auto calendars = t_state->calendars;
    for (auto& tc : calendars) {
        if (auto cv = tc->view.get()) {
            try {
                ApplyCalendarSettings(*tc, cv);
                EnsureInfoPanel(*tc, cv);
            } catch (...) {
                RecordError(L"RefreshCalendar", winrt::to_hresult());
            }
        }
    }
    auto items = t_state->items;
    for (auto& ti : items) {
        if (auto i = ti->item.get()) {
            try {
                UpdateDayItem(i);
            } catch (...) {
                RecordError(L"RefreshItem", winrt::to_hresult());
            }
        }
    }
}

void RefreshIfSettingsChanged() {
    if (ReloadSettingsIfChanged()) {
        RefreshCurrentThread();
    }
}

// Chẩn đoán (chỉ khi debugLog=1): ghi cây cha của CalendarView ra log.
std::wstring DescribeElement(wux::DependencyObject const& o) {
    std::wstring s = winrt::get_class_name(o).c_str();
    if (auto fe = o.try_as<wux::FrameworkElement>()) {
        if (!fe.Name().empty()) {
            s += L" #" + std::wstring(fe.Name());
        }
        WCHAR b[96];
        swprintf_s(b, L" [%.0fx%.0f]", fe.ActualWidth(), fe.ActualHeight());
        s += b;
        if (wuxm::VisualTreeHelper::GetParent(o).try_as<wuxc::Grid>()) {
            s += L" row=" + std::to_wstring(wuxc::Grid::GetRow(fe));
        }
    }
    if (auto g = o.try_as<wuxc::Grid>()) {
        s += L" rows=" + std::to_wstring(g.RowDefinitions().Size());
    }
    if (auto sp = o.try_as<wuxc::StackPanel>()) {
        s += sp.Orientation() == wuxc::Orientation::Vertical ? L" (dọc)"
                                                             : L" (ngang)";
    }
    s += L" con=" +
         std::to_wstring(wuxm::VisualTreeHelper::GetChildrenCount(o));
    return s;
}

void DumpCalendarAncestors(wuxc::CalendarView const& cv) {
    static bool dumped = false;
    if (!g_debugLog || dumped) {
        return;
    }
    dumped = true;
    LOG(L"--- Cây cha của CalendarView ---");
    wux::DependencyObject cur = cv;
    for (int depth = 0; cur && depth < 14; depth++) {
        LOG(L"%d: %ls", depth, DescribeElement(cur).c_str());
        if (depth > 0 && depth <= 8) {
            int n = wuxm::VisualTreeHelper::GetChildrenCount(cur);
            for (int i = 0; i < n && i < 8; i++) {
                LOG(L"     - %ls",
                    DescribeElement(wuxm::VisualTreeHelper::GetChild(cur, i))
                        .c_str());
            }
        }
        cur = wuxm::VisualTreeHelper::GetParent(cur);
    }
}

// Tự kiểm tra: vài giây sau khi thấy bảng lịch, xem đã gắn được âm lịch chưa.
// Trả về true nếu cần kiểm tra lại (bảng lịch đang bị thu gọn).
bool EvaluateSelfTest() {
    if (t_state) {
        auto calendars = t_state->calendars;
        for (auto& tc : calendars) {
            if (auto cv = tc->view.get()) {
                DumpCalendarAncestors(cv);
                if (!tc->infoPanel.get()) {
                    try {
                        EnsureInfoPanel(*tc, cv);  // thử lại nếu lần đầu chưa được
                    } catch (...) {
                        RecordError(L"InfoPanel", winrt::to_hresult());
                    }
                }
            }
        }
    }
    int dayItems, errors;
    std::wstring lastError, state;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        dayItems = g_status.dayItems;
        errors = g_status.errors;
        lastError = g_status.lastError;
        state = g_status.state;
    }

    if (dayItems > 0) {
        if (state != L"OK") {
            SetState(L"OK", L"Đã gắn âm lịch vào " + std::to_wstring(dayItems) +
                                L" ô ngày");
        }
        return false;
    }

    // Chỉ coi là lỗi khi bảng lịch thực sự đang hiển thị (không thu gọn).
    bool visible = false;
    if (t_state) {
        for (auto& tc : t_state->calendars) {
            if (auto cv = tc->view.get()) {
                if (cv.Visibility() == wux::Visibility::Visible &&
                    cv.ActualHeight() > 0 && cv.ActualWidth() > 0) {
                    visible = true;
                }
            }
        }
    }
    if (!visible) {
        return true;
    }

    std::wstring detail =
        L"Tìm thấy bảng lịch nhưng không gắn được âm lịch vào ô ngày nào";
    if (errors > 0) {
        detail += L" (lỗi cuối: " + lastError + L")";
    }
    SetState(L"FAIL", detail);
    return false;
}

void StartSelfTestTimer() {
    auto& st = GetThreadState();
    if (!st.dispatcherQueue) {
        return;
    }
    if (!st.selfTestTimer) {
        st.selfTestTimer = st.dispatcherQueue.CreateTimer();
        st.selfTestTimer.IsRepeating(false);
        st.selfTestTimer.Interval(std::chrono::seconds{4});
        st.selfTestTimer.Tick(
            [](winrt::Windows::System::DispatcherQueueTimer const& timer,
               wf::IInspectable const&) {
                try {
                    if (EvaluateSelfTest()) {
                        timer.Start();  // kiểm tra lại sau 4 giây
                    }
                } catch (...) {
                    RecordError(L"SelfTest", winrt::to_hresult());
                }
            });
    }
    if (!st.selfTestTimer.IsRunning()) {
        st.selfTestTimer.Start();
    }
}

void ProcessCalendarView(wuxc::CalendarView const& cv) {
    auto& st = GetThreadState();

    std::shared_ptr<TrackedCalendar> tc;
    for (size_t i = 0; i < st.calendars.size();) {
        auto alive = st.calendars[i]->view.get();
        if (!alive) {
            st.calendars.erase(st.calendars.begin() + i);
            continue;
        }
        if (alive == cv) {
            tc = st.calendars[i];
        }
        i++;
    }

    if (!tc) {
        tc = std::make_shared<TrackedCalendar>();
        tc->view = winrt::make_weak(cv);
        std::weak_ptr<TrackedCalendar> weakTc = tc;
        tc->dayItemChangingToken = cv.CalendarViewDayItemChanging(
            [weakTc](wuxc::CalendarView const& sender,
                     wuxc::CalendarViewDayItemChangingEventArgs const& args) {
                try {
                    RefreshIfSettingsChanged();
                    if (!args.InRecycleQueue() && args.Item()) {
                        ProcessDayItem(args.Item());
                    }
                    // Sang ngày mới khi bảng lịch đang mở: cập nhật khung.
                    auto t = weakTc.lock();
                    SYSTEMTIME now;
                    GetLocalTime(&now);
                    if (t && t->infoDay != 0 && t->infoDay != now.wDay) {
                        EnsureInfoPanel(*t, sender);
                    }
                } catch (...) {
                    RecordError(L"CalendarViewDayItemChanging",
                                winrt::to_hresult());
                }
            });
        tc->selectedDatesToken = cv.SelectedDatesChanged(
            [weakTc](wuxc::CalendarView const& sender,
                     wuxc::CalendarViewSelectedDatesChangedEventArgs const&) {
                try {
                    if (auto t = weakTc.lock()) {
                        if (auto panel = t->infoPanel.get()) {
                            UpdateInfoPanel(panel, sender);
                        }
                    }
                } catch (...) {
                    RecordError(L"SelectedDatesChanged", winrt::to_hresult());
                }
            });
        st.calendars.push_back(tc);

        int count;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            count = ++g_status.calendarViews;
            WriteStatusFileLocked();
        }
        LOG(L"Tìm thấy CalendarView thứ %d", count);
    }

    ApplyCalendarSettings(*tc, cv);

    // Thêm khung thông tin sau khi XAML xử lý xong đợt thêm phần tử hiện tại.
    if (st.dispatcherQueue) {
        st.dispatcherQueue.TryEnqueue(
            [weakTc = std::weak_ptr<TrackedCalendar>(tc),
             weakView = winrt::make_weak(cv)] {
                try {
                    auto t = weakTc.lock();
                    auto view = weakView.get();
                    if (t && view) {
                        EnsureInfoPanel(*t, view);
                    }
                } catch (...) {
                    RecordError(L"InfoPanel", winrt::to_hresult());
                }
            });
    }
    StartSelfTestTimer();
}

void NoteCalendarLikeType(PCWSTR type) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    for (const auto& t : g_status.calendarLikeTypes) {
        if (t == type) {
            return;
        }
    }
    if (g_status.calendarLikeTypes.size() < 15) {
        g_status.calendarLikeTypes.push_back(type);
        WriteStatusFileLocked();
    }
}

void OnElementAdded(wf::IInspectable const& element,
                    PCWSTR type,
                    wf::IInspectable const& parent) {
    if (wcsstr(type, L"Calendar")) {
        NoteCalendarLikeType(type);
        RefreshIfSettingsChanged();

        if (auto item = element.try_as<wuxc::CalendarViewDayItem>()) {
            ProcessDayItem(item);
        } else if (auto cv = element.try_as<wuxc::CalendarView>()) {
            ProcessCalendarView(cv);
        }
        return;
    }

    // TextBlock số ngày được thêm sau template: cập nhật lại để lấy màu chữ.
    if (parent) {
        if (auto item = parent.try_as<wuxc::CalendarViewDayItem>()) {
            if (FindTrackedItem(item)) {
                UpdateDayItem(item);
            } else {
                ProcessDayItem(item);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// DllMain
////////////////////////////////////////////////////////////////////////////////

// Khởi tạo một lần, khi Windows tạo TAP (không làm trong DllMain vì đang giữ
// loader lock).
void EnsureInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        // Bản MSIX: DLL nằm trong <LocalState>\tap, dữ liệu ở <LocalState>\data.
        WCHAR dllPath[MAX_PATH] = L"";
        GetModuleFileNameW(g_hModule, dllPath, ARRAYSIZE(dllPath));
        g_dataDir = amlich::PackagedDataDirFromTapPath(dllPath);
        if (g_dataDir.empty()) {
            WCHAR programData[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"ProgramData", programData,
                                              ARRAYSIZE(programData));
            if (n == 0 || n >= ARRAYSIZE(programData)) {
                wcscpy_s(programData, L"C:\\ProgramData");
            }
            g_dataDir = std::wstring(programData) + L"\\" AMLICH_DATA_DIR_NAME;
        }
        g_userSid = amlich::CurrentUserSidString();

        WCHAR exePath[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath));
        PCWSTR name = L"unknown.exe";
        if (len > 0 && len < ARRAYSIZE(exePath)) {
            PCWSTR slash = wcsrchr(exePath, L'\\');
            name = slash ? slash + 1 : exePath;
        }
        g_processName = name;

        LoadSettings();
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard,
                                 &data)) {
            g_settingsWriteTime = data.ftLastWriteTime;
        }

        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.osBuild = ReadOsBuild();
            WriteStatusFileLocked();
        }
        LOG(L"Đã nạp vào %ls (pid %lu), Windows build %ls",
            g_processName.c_str(), GetCurrentProcessId(),
            g_status.osBuild.c_str());
    });
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);

        // Ghim DLL: không bao giờ bị gỡ khỏi bộ nhớ khi shell còn chạy (đã có
        // callback gắn vào giao diện).
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    }
    return TRUE;
}
