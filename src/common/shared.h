#pragma once

// Hằng số và hàm dùng chung giữa AmLichTap.dll và AmLichVN.exe.

#include <windows.h>

#include <string>

#include "vnlunar.h"

#ifndef AMLICH_VERSION
#define AMLICH_VERSION L"1.2.0"
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
