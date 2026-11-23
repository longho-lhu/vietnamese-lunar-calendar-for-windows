#pragma once

// Hằng số và hàm dùng chung giữa AmLichTap.dll và AmLichVN.exe.

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <string>

#include "vnlunar.h"

#ifndef AMLICH_VERSION
#define AMLICH_VERSION L"1.3.0"
#endif
#define AMLICH_DATA_DIR_NAME L"AmLichVN"  // trong %ProgramData%
#define AMLICH_SETTINGS_FILE L"settings.ini"
#define AMLICH_INI_SECTION L"AmLich"
#define AMLICH_TAP_DLL L"AmLichTap.dll"

// {5B0C7A3E-9D41-4C2A-8F6E-317D52A90B14}
static constexpr CLSID CLSID_AmLichTap = {
    0x5b0c7a3e,
    0x9d41,
    0x4c2a,
    {0x8f, 0x6e, 0x31, 0x7d, 0x52, 0xa9, 0x0b, 0x14}};

namespace amlich {

// SID (dạng chuỗi) của người dùng sở hữu tiến trình hiện tại. Trong tiến trình
// AppContainer (bảng lịch) TokenUser vẫn là SID của người dùng.
inline std::wstring CurrentUserSidString() {
    std::wstring result;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return result;
    }
    alignas(TOKEN_USER) BYTE buf[256];
    DWORD len = 0;
    if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len)) {
        LPWSTR str = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid,
                                   &str)) {
            result = str;
            LocalFree(str);
        }
    }
    CloseHandle(token);
    return result;
}

// Handle trỏ tới một file thường (không phải thư mục / reparse point), chỉ có
// một tên (không phải hard link) và do người dùng của tiến trình hiện tại sở
// hữu. Handle cần FILE_READ_ATTRIBUTES và READ_CONTROL.
inline bool IsOwnSingleLinkFile(HANDLE file) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
                                      sizeof(tag)) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        return false;
    }
    FILE_STANDARD_INFO si{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &si, sizeof(si)) ||
        si.NumberOfLinks != 1) {
        return false;
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                        nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    HANDLE token = nullptr;
    if (owner && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_USER) BYTE buf[256];
        DWORD len = 0;
        if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len)) {
            ok = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(buf)->User.Sid) != FALSE;
        }
        CloseHandle(token);
    }
    LocalFree(sd);
    return ok;
}

// File trạng thái / log của DLL nằm trong <dữ liệu>\status và mang SID người
// dùng trong tên: mỗi người chỉ ghi được (và AmLichVN.exe chỉ tin) file của
// chính mình.
inline std::wstring StatusFilePath(const std::wstring& dataDir,
                                   const std::wstring& sid,
                                   const std::wstring& process) {
    return dataDir + L"\\status\\status-" + sid + L"-" + process + L".txt";
}

inline std::wstring DllLogFilePath(const std::wstring& dataDir,
                                   const std::wstring& sid,
                                   const std::wstring& process) {
    return dataDir + L"\\status\\log-" + sid + L"-" + process + L".txt";
}

// Bản MSIX: thư mục dữ liệu là <LocalState của gói>\data (riêng từng người
// dùng). AmLichTap.dll được chép vào <LocalState>\tap nên suy ra được từ
// đường dẫn của chính nó; trả về chuỗi rỗng nếu không phải trường hợp đó.
inline std::wstring PackagedDataDirFromTapPath(const std::wstring& dllPath) {
    size_t pos = dllPath.rfind(L"\\LocalState\\tap\\");
    if (pos == std::wstring::npos) {
        return L"";
    }
    return dllPath.substr(0, pos) + L"\\LocalState\\data";
}

inline std::wstring MonthName(int m) {
    if (m == 1) {
        return L"Giêng";
    }
    if (m == 12) {
        return L"Chạp";
    }
    return std::to_wstring(m);
}

// Mô tả đầy đủ một ngày âm lịch (dùng cho chú thích khi rê chuột).
inline std::wstring BuildDescription(const vnlunar::LunarDate& l) {
    std::wstring s;
    s += L"Âm lịch: ngày " + std::to_wstring(l.day) + L" tháng " +
         MonthName(l.month) + (l.leap ? L" (nhuận)" : L"") + L" năm " +
         vnlunar::YearCanChi(l.year);
    s += l.monthLength == 30 ? L" (tháng đủ)" : L" (tháng thiếu)";
    s += L"\nNgày " + vnlunar::DayCanChi(l.jd) + L", tháng " +
         vnlunar::MonthCanChi(l.month, l.year) + L", năm " +
         vnlunar::YearCanChi(l.year);
    s += L"\nTiết khí: ";
    s += vnlunar::kSolarTerms[vnlunar::SolarTermIndex(l.jd)];
    if (vnlunar::SolarTermStartsOn(l.jd)) {
        s += L" (bắt đầu hôm nay)";
    }
    s += L"\nGiờ hoàng đạo: " + vnlunar::GioHoangDao(l.jd);
    if (const wchar_t* holiday = vnlunar::LunarHoliday(l)) {
        s += L"\n";
        s += holiday;
    }
    return s;
}

}  // namespace amlich
