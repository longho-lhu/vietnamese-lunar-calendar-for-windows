# Ghi chú cho người phát triển

| Thành phần | Vai trò |
|---|---|
| `src/tap/AmLichTap.cpp` → `AmLichTap.dll` | Được nạp vào tiến trình bảng lịch (ShellHost.exe / ShellExperienceHost.exe) qua `InitializeXamlDiagnosticsEx`. Gắn dòng âm lịch vào từng ô `CalendarViewDayItem`, thêm khung thông tin ngày, tự ghi trạng thái |
| `src/app/AmLichVN.cpp` → `AmLichVN.exe` | Ứng dụng chạy nền (icon khay): nạp DLL, cửa sổ Cài đặt, tự kiểm tra, cập nhật |
| `src/app/updater.h` | Cập nhật qua GitHub Releases: tải HTTPS, kiểm tra chữ ký số ECDSA P-256, cài đặt |
| `src/app/datadir.h` | Tạo / sửa quyền / xóa `%ProgramData%\AmLichVN` (`--secure-datadir`, `--remove-datadir`) |
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

Kiểm tra cập nhật từ dòng lệnh: `AmLichVN.exe --check-update` (kết quả ghi vào `%LOCALAPPDATA%\AmLichVN\app-log.txt`).

## Thư mục dữ liệu và quyền
`%ProgramData%\AmLichVN` dùng chung cho mọi người dùng và cho DLL trong tiến trình bảng lịch (AppContainer). Các tiến trình quyền Administrator (bộ cài, gỡ cài, bước cài bản cập nhật) cũng đụng tới thư mục này, nên nó phải không cho người dùng thường đặt junction / symlink / hard link vào:

| Đối tượng | Chủ sở hữu | Quyền (DACL được bảo vệ, không kế thừa) |
|---|---|---|
| thư mục gốc | Administrators | SYSTEM, Administrators: F · Users, ALL APPLICATION PACKAGES: RX |
| `settings.ini` | Administrators | Users: đọc + ghi nội dung (không xóa) · ALL APPLICATION PACKAGES: đọc |
| `status\` | Administrators | Users, ALL APPLICATION PACKAGES: chỉ tạo file · CREATOR OWNER: F trên file · ALL APPLICATION PACKAGES: đọc/ghi trên file |

- Chỉ `AmLichVN.exe --secure-datadir` (quyền Admin) tạo / sửa thư mục này. Nó mở mọi thứ tương đối từ handle (`NtCreateFile` + `RootDirectory`, `FILE_OPEN_REPARSE_POINT`), đặt ACL trực tiếp lên handle và xóa mọi mục lạ. `install.ps1` gọi lệnh này; bước cài bản cập nhật cũng gọi sau khi cài xong. `--remove-datadir` dùng khi gỡ cài.
- File trạng thái / log của DLL: `status\status-<SID>-<tiến trình>.txt`, `status\log-<SID>-<tiến trình>.txt`. DLL và exe chỉ dùng file là file thường, một tên, do chính người dùng sở hữu.
- Log của `AmLichVN.exe` nằm riêng từng người dùng ở `%LOCALAPPDATA%\AmLichVN\app-log.txt`. Tiến trình quyền Admin không ghi log, mà trả kết quả qua mã thoát.
- Bản cài từ trước 1.3.0 có thư mục với quyền cũ (Users: Modify). Khi khởi động, exe phát hiện điều này và hiện mục menu "Sửa quyền thư mục dữ liệu". Ngay sau khi cập nhật OTA, exe hỏi luôn (một lần UAC).

## Gói MSIX (Microsoft Store)
```powershell
.\build.cmd
.\tools\build-msix.ps1 -IdentityName <Package/Identity/Name> -Publisher "<Package/Identity/Publisher>" -PublisherDisplayName "<tên hiển thị>"
```
Các giá trị lấy trong Partner Center (Product management → Product identity). Gói gửi lên Store **không cần ký**, vì Store tự ký lại. Manifest mẫu: `packaging/msix/AppxManifest.xml`, khai báo `runFullTrust` và `StartupTask`.

Khi chạy trong gói, `AmLichVN.exe` tự nhận biết (`IsPackaged()`) và:
- Chép `AmLichTap.dll` sang `%LOCALAPPDATA%\Packages\<gói>\LocalState\tap`, rồi cấp quyền chỉ đọc cho ALL APPLICATION PACKAGES. Lý do: tiến trình bảng lịch là AppContainer, không đọc được file trong `WindowsApps`. Không đặt DLL ở `ProgramData`, vì mọi người dùng ghi được thư mục đó.
- Dùng thư mục dữ liệu riêng của người dùng `<LocalState>\data` (không dùng `ProgramData`). Tự tạo thư mục này, cấp quyền đọc/ghi cho ALL APPLICATION PACKAGES, và tạo `settings.ini` mặc định. DLL tự suy ra thư mục này từ đường dẫn của chính nó (`LocalState\tap\...`). Log nằm ở `<LocalState>\app-log.txt`.
- Khởi động cùng Windows bằng `StartupTask`, vì khóa `Run` bị ảo hóa trong gói.
- Tắt cơ chế cập nhật qua GitHub: bản Store cập nhật qua Store.

Đã thử trên Windows build 26300 (cài gói vào `WindowsApps`, giả lập máy mới): âm lịch, khung thông tin ngày, cửa sổ Cài đặt và StartupTask đều chạy. Lần thử đó máy vẫn có sẵn khóa `HKLM\...\XAML\Debug\DisableCompositionDiag = 1` từ bản cài thường. Chưa thử trên máy không có khóa này.

## Ký số
- Mỗi Release có `manifest.txt` (phiên bản, kích thước + SHA-256 từng file) và `manifest.sig` (chữ ký ECDSA P-256 của manifest).
- Ứng dụng chỉ cài khi chữ ký khớp khóa công khai biên dịch sẵn, phiên bản **mới hơn** bản đang cài, và SHA-256 từng file khớp manifest. Bước cài chạy với quyền Administrator: nó đọc file vào bộ nhớ rồi tự kiểm tra lại từ đầu.
- Khóa riêng **không** nằm trong repo. Nó ở GitHub Secret `AMLICH_SIGNING_KEY` và trong bản sao lưu của người phát triển (`%USERPROFILE%\.amlichvn-signing\signing-key.txt`).
- Mất khóa riêng: chạy `tools\new-signing-key.ps1 -Force`, cập nhật secret, phát hành bản mới. Các máy đã cài phải cài tay bản đó một lần.
