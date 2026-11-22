# Ghi chú cho người phát triển

| Thành phần | Vai trò |
|---|---|
| `src/tap/AmLichTap.cpp` → `AmLichTap.dll` | Được nạp vào tiến trình bảng lịch (ShellHost.exe / ShellExperienceHost.exe) qua `InitializeXamlDiagnosticsEx`. Gắn dòng âm lịch vào từng ô `CalendarViewDayItem`, thêm khung thông tin ngày, tự ghi trạng thái |
| `src/app/AmLichVN.cpp` → `AmLichVN.exe` | Ứng dụng chạy nền (icon khay): nạp DLL, cửa sổ Cài đặt, tự kiểm tra, cập nhật |
| `src/app/updater.h` | Cập nhật qua GitHub Releases: tải HTTPS, kiểm tra chữ ký số ECDSA P-256, cài đặt |
| `src/common/vnlunar.h` | Thuật toán âm lịch (Hồ Ngọc Đức), can chi, tiết khí, giờ hoàng đạo, ngày lễ |
| `src/common/update_pubkey.h` | Khóa công khai dùng để kiểm tra chữ ký bản cập nhật |
| `scripts/` | `CaiDat.cmd`, `GoCaiDat.cmd`, `KiemTra.cmd` (+ các file .ps1 tương ứng) |
| `tools/` | `new-signing-key.ps1` (tạo khóa), `sign-release.ps1` (CI ký bản phát hành), `release.ps1` (phát hành) |
| `settings.default.ini` | Cài đặt hiển thị mặc định |

## Biên dịch
Cài Visual Studio 2022 (hoặc Build Tools) với workload **Desktop development with C++**, rồi chạy `build.cmd`. Kết quả nằm trong `dist\`.

## Phát hành phiên bản mới
```powershell
.\tools\release.ps1 -Version 1.3.0 -Notes "Mô tả thay đổi"
```
Script tăng `AMLICH_VERSION` trong `src/common/shared.h`, commit, tạo tag `v1.3.0` rồi push. GitHub Actions sẽ build, ký và tạo Release.

Kiểm tra cập nhật từ dòng lệnh: `AmLichVN.exe --check-update` (kết quả ghi vào `app-log.txt`).

## Ký số
- Mỗi Release có `manifest.txt` (phiên bản, kích thước + SHA-256 từng file) và `manifest.sig` (chữ ký ECDSA P-256 của manifest).
- Ứng dụng chỉ cài khi chữ ký khớp khóa công khai biên dịch sẵn, phiên bản **mới hơn** bản đang cài, và SHA-256 từng file khớp manifest. Bước cài chạy với quyền Administrator: nó đọc file vào bộ nhớ rồi tự kiểm tra lại từ đầu.
- Khóa riêng **không** nằm trong repo. Nó ở GitHub Secret `AMLICH_SIGNING_KEY` và trong bản sao lưu của người phát triển (`%USERPROFILE%\.amlichvn-signing\signing-key.txt`).
- Mất khóa riêng: chạy `tools\new-signing-key.ps1 -Force`, cập nhật secret, phát hành bản mới. Các máy đã cài phải cài tay bản đó một lần.
