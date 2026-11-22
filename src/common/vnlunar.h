#pragma once

#include <cmath>
#include <string>

////////////////////////////////////////////////////////////////////////////////
// Âm lịch Việt Nam - thuật toán của Hồ Ngọc Đức (tính theo múi giờ UTC+7).
// Nguồn thuật toán: https://www.informatik.uni-leipzig.de/~duc/amlich/
// Phần này không phụ thuộc Windows, có thể biên dịch/kiểm thử riêng.
////////////////////////////////////////////////////////////////////////////////

namespace vnlunar {

constexpr double kPi = 3.14159265358979323846;

inline long FloorL(double d) {
    return (long)std::floor(d);
}

// Số ngày Julius (JDN) của một ngày dương lịch.
inline long JdFromDate(int dd, int mm, int yy) {
    long a = (14 - mm) / 12;
    long y = yy + 4800 - a;
    long m = mm + 12 * a - 3;
    long jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 -
              32045;
    if (jd < 2299161) {
        jd = dd + (153 * m + 2) / 5 + 365 * y + y / 4 - 32083;
    }
    return jd;
}

// Đổi JDN ngược lại thành ngày dương lịch.
inline void JdToDate(long jd, int* dd, int* mm, int* yy) {
    long a, b, c;
    if (jd > 2299160) {
        a = jd + 32044;
        b = (4 * a + 3) / 146097;
        c = a - (b * 146097) / 4;
    } else {
        b = 0;
        c = jd + 32082;
    }
    long d = (4 * c + 3) / 1461;
    long e = c - (1461 * d) / 4;
    long m = (5 * e + 2) / 153;
    *dd = (int)(e - (153 * m + 2) / 5 + 1);
    *mm = (int)(m + 3 - 12 * (m / 10));
    *yy = (int)(b * 100 + d - 4800 + m / 10);
}

// Thời điểm (JD, giờ UT) của điểm sóc thứ k tính từ 1/1/1900.
inline double NewMoon(long k) {
    double T = k / 1236.85;
    double T2 = T * T;
    double T3 = T2 * T;
    double dr = kPi / 180;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 -
                 0.000000155 * T3;
    Jd1 = Jd1 + 0.00033 * std::sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr =
        306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 =
        (0.1734 - 0.000393 * T) * std::sin(M * dr) + 0.0021 * std::sin(2 * dr * M);
    C1 = C1 - 0.4068 * std::sin(Mpr * dr) + 0.0161 * std::sin(dr * 2 * Mpr);
    C1 = C1 - 0.0004 * std::sin(dr * 3 * Mpr);
    C1 = C1 + 0.0104 * std::sin(dr * 2 * F) - 0.0051 * std::sin(dr * (M + Mpr));
    C1 = C1 - 0.0074 * std::sin(dr * (M - Mpr)) +
         0.0004 * std::sin(dr * (2 * F + M));
    C1 = C1 - 0.0004 * std::sin(dr * (2 * F - M)) -
         0.0006 * std::sin(dr * (2 * F + Mpr));
    C1 = C1 + 0.0010 * std::sin(dr * (2 * F - Mpr)) +
         0.0005 * std::sin(dr * (2 * Mpr + M));
    double deltat;
    if (T < -11) {
        deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 -
                 0.000000081 * T * T3;
    } else {
        deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
    }
    return Jd1 + C1 - deltat;
}

// Kinh độ mặt trời (radian, 0..2π) tại thời điểm jdn (JD, giờ UT).
inline double SunLongitude(double jdn) {
    double T = (jdn - 2451545.0) / 36525;
    double T2 = T * T;
    double dr = kPi / 180;
    double M =
        357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * std::sin(dr * M);
    DL = DL + (0.019993 - 0.000101 * T) * std::sin(dr * 2 * M) +
         0.000290 * std::sin(dr * 3 * M);
    double L = (L0 + DL) * dr;
    L = L - kPi * 2 * (FloorL(L / (kPi * 2)));
    return L;
}

inline long GetNewMoonDay(long k, double tz) {
    return FloorL(NewMoon(k) + 0.5 + tz / 24);
}

// Cung hoàng đạo (0..11, mỗi cung 30°) của mặt trời lúc 0h đầu ngày.
inline int GetSunLongitude(long dayNumber, double tz) {
    return (int)FloorL(SunLongitude(dayNumber - 0.5 - tz / 24) / kPi * 6);
}

// Ngày bắt đầu tháng 11 âm lịch (tháng chứa Đông chí) của năm yy.
inline long GetLunarMonth11(int yy, double tz) {
    long off = JdFromDate(31, 12, yy) - 2415021;
    long k = FloorL(off / 29.530588853);
    long nm = GetNewMoonDay(k, tz);
    int sunLong = GetSunLongitude(nm, tz);
    if (sunLong >= 9) {
        nm = GetNewMoonDay(k - 1, tz);
    }
    return nm;
}

inline int GetLeapMonthOffset(long a11, double tz) {
    long k = FloorL((a11 - 2415021.076998695) / 29.530588853 + 0.5);
    int last = 0;
    int i = 1;
    int arc = GetSunLongitude(GetNewMoonDay(k + i, tz), tz);
    do {
        last = arc;
        i++;
        arc = GetSunLongitude(GetNewMoonDay(k + i, tz), tz);
    } while (arc != last && i < 14);
    return i - 1;
}

struct LunarDate {
    int day = 0;
    int month = 0;
    int year = 0;
    bool leap = false;
    int monthLength = 0;  // 29 (tháng thiếu) hoặc 30 (tháng đủ)
    long jd = 0;          // JDN của ngày dương lịch tương ứng
};

inline LunarDate SolarToLunar(int dd, int mm, int yy, double tz = 7.0) {
    LunarDate r;
    long dayNumber = JdFromDate(dd, mm, yy);
    r.jd = dayNumber;
    long k = FloorL((dayNumber - 2415021.076998695) / 29.530588853);
    long kStart = k + 1;
    long monthStart = GetNewMoonDay(kStart, tz);
    // Bản gốc chỉ lùi 1 lần; một vài ngày hiếm (vd. 7/5/2054) cần lùi thêm,
    // nếu không sẽ ra "ngày 0".
    while (monthStart > dayNumber) {
        kStart--;
        monthStart = GetNewMoonDay(kStart, tz);
    }
    long a11 = GetLunarMonth11(yy, tz);
    long b11 = a11;
    int lunarYear;
    if (a11 >= monthStart) {
        lunarYear = yy;
        a11 = GetLunarMonth11(yy - 1, tz);
    } else {
        lunarYear = yy + 1;
        b11 = GetLunarMonth11(yy + 1, tz);
    }
    int lunarDay = (int)(dayNumber - monthStart + 1);
    int diff = (int)FloorL((monthStart - a11) / 29.0);
    bool lunarLeap = false;
    int lunarMonth = diff + 11;
    if (b11 - a11 > 365) {
        int leapMonthDiff = GetLeapMonthOffset(a11, tz);
        if (diff >= leapMonthDiff) {
            lunarMonth = diff + 10;
            if (diff == leapMonthDiff) {
                lunarLeap = true;
            }
        }
    }
    if (lunarMonth > 12) {
        lunarMonth = lunarMonth - 12;
    }
    if (lunarMonth >= 11 && diff < 4) {
        lunarYear -= 1;
    }
    r.day = lunarDay;
    r.month = lunarMonth;
    r.year = lunarYear;
    r.leap = lunarLeap;
    r.monthLength = (int)(GetNewMoonDay(kStart + 1, tz) - monthStart);
    return r;
}

// Tiết khí đang có hiệu lực vào ngày jd (0 = Xuân phân, mỗi tiết 15°).
inline int SolarTermIndex(long jd, double tz = 7.0) {
    double L = SunLongitude(jd - 0.5 - tz / 24);  // lúc 0h đầu ngày
    int idx = (int)FloorL(L / kPi * 12);
    if (idx < 0) {
        idx = 0;
    }
    if (idx > 23) {
        idx = 23;
    }
    // Nếu trong ngày mặt trời bước sang tiết mới thì coi ngày đó là ngày
    // bắt đầu tiết mới (giống cách các lịch Việt in).
    double L2 = SunLongitude(jd + 0.5 - tz / 24);  // lúc 24h cuối ngày
    int idx2 = (int)FloorL(L2 / kPi * 12);
    if (idx2 < 0) {
        idx2 = 0;
    }
    if (idx2 > 23) {
        idx2 = 23;
    }
    return idx2 != idx ? idx2 : idx;
}

// Ngày jd có phải ngày bắt đầu một tiết khí không.
inline bool SolarTermStartsOn(long jd, double tz = 7.0) {
    return SolarTermIndex(jd, tz) != SolarTermIndex(jd - 1, tz);
}

inline const wchar_t* const kCan[10] = {L"Giáp", L"Ất",  L"Bính", L"Đinh",
                                        L"Mậu",  L"Kỷ",  L"Canh", L"Tân",
                                        L"Nhâm", L"Quý"};
inline const wchar_t* const kChi[12] = {L"Tý",   L"Sửu", L"Dần", L"Mão",
                                        L"Thìn", L"Tỵ",  L"Ngọ", L"Mùi",
                                        L"Thân", L"Dậu", L"Tuất", L"Hợi"};
inline const wchar_t* const kSolarTerms[24] = {
    L"Xuân phân", L"Thanh minh", L"Cốc vũ",     L"Lập hạ",   L"Tiểu mãn",
    L"Mang chủng", L"Hạ chí",    L"Tiểu thử",   L"Đại thử",  L"Lập thu",
    L"Xử thử",    L"Bạch lộ",    L"Thu phân",   L"Hàn lộ",   L"Sương giáng",
    L"Lập đông",  L"Tiểu tuyết", L"Đại tuyết",  L"Đông chí", L"Tiểu hàn",
    L"Đại hàn",   L"Lập xuân",   L"Vũ thủy",    L"Kinh trập"};

inline std::wstring YearCanChi(int lunarYear) {
    return std::wstring(kCan[(lunarYear + 6) % 10]) + L" " +
           kChi[(lunarYear + 8) % 12];
}

// Can chi của tháng (tháng nhuận dùng can chi của tháng chính).
inline std::wstring MonthCanChi(int lunarMonth, int lunarYear) {
    return std::wstring(kCan[(lunarYear * 12 + lunarMonth + 3) % 10]) + L" " +
           kChi[(lunarMonth + 1) % 12];
}

inline std::wstring DayCanChi(long jd) {
    return std::wstring(kCan[(jd + 9) % 10]) + L" " + kChi[(jd + 1) % 12];
}

// Giờ hoàng đạo theo chi của ngày.
inline std::wstring GioHoangDao(long jd) {
    static const char* const kPattern[6] = {
        "110100101100", "001101001011", "110011010010",
        "101100110100", "001011001101", "010010110011"};
    int chiOfDay = (int)((jd + 1) % 12);
    const char* p = kPattern[chiOfDay % 6];
    std::wstring s;
    for (int i = 0; i < 12; i++) {
        if (p[i] == '1') {
            if (!s.empty()) {
                s += L", ";
            }
            s += kChi[i];
        }
    }
    return s;
}

// Tên ngày lễ/ngày đặc biệt theo âm lịch (nullptr nếu không có).
inline const wchar_t* LunarHoliday(const LunarDate& d) {
    if (d.leap) {
        return nullptr;
    }
    int m = d.month, n = d.day;
    if (m == 1 && n == 1) return L"Tết Nguyên đán";
    if (m == 1 && n == 2) return L"Mùng 2 Tết";
    if (m == 1 && n == 3) return L"Mùng 3 Tết";
    if (m == 1 && n == 15) return L"Tết Nguyên tiêu (Rằm tháng Giêng)";
    if (m == 3 && n == 3) return L"Tết Hàn thực";
    if (m == 3 && n == 10) return L"Giỗ Tổ Hùng Vương";
    if (m == 4 && n == 15) return L"Lễ Phật đản";
    if (m == 5 && n == 5) return L"Tết Đoan ngọ";
    if (m == 7 && n == 15) return L"Lễ Vu lan";
    if (m == 8 && n == 15) return L"Tết Trung thu";
    if (m == 9 && n == 9) return L"Tết Trùng cửu";
    if (m == 10 && n == 10) return L"Tết Trùng thập";
    if (m == 12 && n == 23) return L"Ông Công, ông Táo";
    if (m == 12 && n == d.monthLength) return L"Giao thừa (Trừ tịch)";
    return nullptr;
}

}  // namespace vnlunar
