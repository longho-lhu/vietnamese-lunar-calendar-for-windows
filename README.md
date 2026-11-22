# Âm lịch Việt Nam cho lịch Taskbar Windows 11

Hiện ngày âm lịch dưới mỗi ngày trong bảng lịch mở ra khi bấm vào đồng hồ trên taskbar. Dưới cùng bảng lịch có thêm khung thông tin ngày: can chi, tiết khí, giờ hoàng đạo, ngày lễ.

Yêu cầu: Windows 11.

## Cài đặt
1. Vào trang [Releases](https://github.com/longho-lhu/vietnamese-lunar-calendar-for-windows/releases/latest), tải file `AmLichVN-x.y.z.zip`.
2. Giải nén ra một thư mục.
3. Chạy `CaiDat.cmd`, rồi bấm **Yes** khi Windows hỏi quyền Administrator.
4. Khi cửa sổ cài đặt báo **XONG**, bấm vào đồng hồ ở góc phải taskbar để mở lịch.

Nếu Windows SmartScreen cảnh báo file tải từ Internet, bấm **More info** (Thêm thông tin) → **Run anyway** (Vẫn chạy).

Sau khi cài, ứng dụng chạy nền với icon ngày âm ở khay hệ thống và tự khởi động cùng Windows. Nếu không thấy âm lịch trong bảng lịch, chạy `KiemTra.cmd` để xem nguyên nhân.

## Cập nhật
**Tự động:** mỗi khi khởi động, ứng dụng tự kiểm tra bản mới.
1. Khi có bản mới, một thông báo sẽ hiện lên. Bấm vào thông báo.
2. Chọn **Cập nhật ngay**, rồi bấm **Yes** khi Windows hỏi quyền Administrator.

Bảng lịch sẽ tắt vài giây rồi tự mở lại với bản mới. Ứng dụng chỉ cài bản có chữ ký số hợp lệ của nhà phát triển.

**Kiểm tra ngay:** bấm chuột trái vào icon âm lịch ở khay → **Kiểm tra ngay**.

**Cài tay:** tải file zip mới nhất ở trang Releases rồi chạy lại `CaiDat.cmd`. Cài đặt cũ của bạn được giữ nguyên.

## Gỡ cài đặt
1. Chạy `GoCaiDat.cmd`. File này có trong thư mục đã giải nén, và cả trong `C:\Program Files\AmLichVN`.
2. Bấm **Yes** khi Windows hỏi quyền Administrator.

Âm lịch sẽ biến mất khỏi bảng lịch. Ứng dụng, cài đặt và log trong `C:\ProgramData\AmLichVN` cũng bị xóa.

## Giấy phép
[GNU GPL v3.0](LICENSE). Phần nạp vào giao diện Windows dựa trên [TranslucentTB](https://github.com/TranslucentTB/TranslucentTB) và mod "Windows 11 Notification Center Styler" của m417z, cả hai theo GPL-3.0. Thuật toán âm lịch: Hồ Ngọc Đức.
