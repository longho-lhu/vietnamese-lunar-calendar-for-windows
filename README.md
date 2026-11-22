# AmLichVN - Âm lịch Việt Nam trong lịch Taskbar Windows 11

Hiện ngày âm lịch Việt Nam (tính theo UTC+7) dưới mỗi ngày trong bảng lịch mở ra khi bấm vào đồng hồ trên taskbar, kèm khung thông tin ngày (can chi, tiết khí, giờ hoàng đạo, ngày lễ) ở dưới cùng bảng lịch.

| Thành phần | Vai trò |
|---|---|
| `src/tap/AmLichTap.cpp` → `AmLichTap.dll` | Được nạp vào tiến trình bảng lịch (ShellHost.exe / ShellExperienceHost.exe) qua `InitializeXamlDiagnosticsEx`, gắn dòng âm lịch vào từng ô `CalendarViewDayItem`, tự ghi trạng thái |
| `src/app/AmLichVN.cpp` → `AmLichVN.exe` | Ứng dụng chạy nền (icon khay hiện ngày âm): nạp DLL, tự nạp lại khi shell khởi động lại, tự kiểm tra sau cập nhật Windows, thông báo lỗi |
| `src/app/updater.h` | Cập nhật qua GitHub Releases: tải HTTPS, kiểm tra chữ ký số ECDSA P-256, cài đặt |
| `src/common/vnlunar.h` | Thuật toán âm lịch (Hồ Ngọc Đức), can chi, tiết khí, giờ hoàng đạo, ngày lễ |
| `src/common/update_pubkey.h` | Khóa công khai dùng để kiểm tra chữ ký bản cập nhật |
| `scripts/` | `CaiDat.cmd`, `GoCaiDat.cmd`, `KiemTra.cmd` (+ các file .ps1 tương ứng) |
| `tools/` | `new-signing-key.ps1` (tạo khóa), `sign-release.ps1` (CI ký bản phát hành), `release.ps1` (phát hành) |
| `settings.default.ini` | Cài đặt hiển thị mặc định |

## Biên dịch
Cài Visual Studio 2022 Community, workload **Desktop development with C++**, rồi chạy `build.cmd`. Kết quả nằm trong thư mục `dist\`.

Hoặc đẩy lên GitHub rồi chạy workflow **Build AmLichVN** trong tab Actions.

## Cài đặt / kiểm tra / gỡ
- `dist\CaiDat.cmd`: cài hoặc cập nhật
- `KiemTra.cmd`: xem trạng thái (dùng sau mỗi lần Windows cập nhật)
- `GoCaiDat.cmd`: gỡ cài đặt

Dữ liệu: `C:\ProgramData\AmLichVN\` (settings.ini, status\, app-log.txt).

## Cài đặt hiển thị
Bấm chuột trái vào icon âm lịch ở khay (hoặc menu chuột phải → **Cài đặt...**) để mở cửa sổ Cài đặt: kiểu hiển thị, cỡ chữ, độ đậm, màu tô ngày lễ, tiết khí, chú thích, khung thông tin ngày, khởi động cùng Windows, cập nhật. Thay đổi được ghi vào `settings.ini` và có hiệu lực khi mở lại bảng lịch.

## Cập nhật tự động (OTA qua GitHub)
Cửa sổ Cài đặt → nhóm **Cập nhật phần mềm**: đổi kho GitHub (mặc định `longho-lhu/vietnamese-lunar-calendar-for-windows`), bật/tắt tự kiểm tra (mỗi khi ứng dụng khởi động), **Kiểm tra ngay**.

Khi có bản mới, ứng dụng hiện thông báo và **luôn hỏi trước khi cài**. Đồng ý thì nó tải về, kiểm tra chữ ký, xin quyền Administrator (UAC), thay file, khởi động lại bảng lịch rồi chạy bản mới.

### Ký số
- Mỗi Release có `manifest.txt` (phiên bản, kích thước + SHA-256 từng file) và `manifest.sig` (chữ ký ECDSA P-256 của manifest).
- Ứng dụng chỉ cài khi chữ ký khớp khóa công khai biên dịch sẵn (`src/common/update_pubkey.h`), phiên bản **mới hơn** bản đang cài, và SHA-256 từng file khớp manifest. Bước cài chạy với quyền Administrator: nó đọc file vào bộ nhớ rồi tự kiểm tra lại từ đầu.
- Khóa riêng **không** nằm trong repo. Nó ở GitHub Secret `AMLICH_SIGNING_KEY` và trong bản sao lưu của người phát triển (`%USERPROFILE%\.amlichvn-signing\signing-key.txt`).
- Mất khóa riêng: chạy `tools\new-signing-key.ps1 -Force`, cập nhật secret, phát hành bản mới. Các máy đã cài phải cài tay bản đó một lần.

### Phát hành phiên bản mới
```powershell
.\tools\release.ps1 -Version 1.2.0 -Notes "Mô tả thay đổi"
```
Script tăng `AMLICH_VERSION` trong `src/common/shared.h`, commit, tạo tag `v1.2.0` rồi push. GitHub Actions sẽ build, ký và tạo Release.

Kiểm tra từ dòng lệnh: `AmLichVN.exe --check-update` (kết quả ghi vào `app-log.txt`).

Giấy phép: GPL-3.0 (phần TAP dựa trên TranslucentTB và Windows 11 Notification Center Styler).
