# Chính sách quyền riêng tư – Âm lịch Việt Nam

*Cập nhật: 23/11/2026*

Âm lịch Việt Nam **không thu thập, không gửi đi và không chia sẻ** bất kỳ dữ liệu cá nhân nào của bạn.

- **Không có tài khoản, quảng cáo hay theo dõi sử dụng (telemetry).**
- **Dữ liệu lưu trên máy bạn:** cài đặt hiển thị và file trạng thái (`C:\ProgramData\AmLichVN`; bản Store: thư mục dữ liệu của gói), log để tìm lỗi (`%LOCALAPPDATA%\AmLichVN`; bản Store: thư mục dữ liệu của gói), cùng một vài giá trị trong registry của tài khoản Windows. Các dữ liệu này không bao giờ rời khỏi máy. Khi gỡ ứng dụng, cài đặt và file trạng thái bị xóa; với bản GitHub, file log trong `%LOCALAPPDATA%\AmLichVN` được giữ lại để bạn tự xóa.
- **Kết nối mạng:**
  - Bản cài từ **Microsoft Store** không kết nối Internet. Việc cập nhật do Microsoft Store đảm nhận.
  - Bản tải từ **GitHub** chỉ kết nối tới `github.com` để kiểm tra và tải bản cập nhật. Yêu cầu gửi đi chỉ chứa thông tin kỹ thuật thông thường của một lần truy cập web (địa chỉ IP, tên phiên bản ứng dụng). Ứng dụng không gửi cookie hay thông tin đăng nhập nào. GitHub xử lý các yêu cầu này theo [chính sách quyền riêng tư của GitHub](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement).
- **Tương tác với Windows:** để hiện âm lịch, ứng dụng nạp một thành phần vào tiến trình bảng lịch của Windows trên chính máy bạn. Thành phần này chỉ đọc và hiển thị ngày tháng, không đọc nội dung khác.

Câu hỏi hoặc góp ý: mở issue tại https://github.com/longho-lhu/vietnamese-lunar-calendar-for-windows/issues

---

## Privacy policy (English)

Âm lịch Việt Nam (Vietnamese Lunar Calendar) **does not collect, transmit or share any personal data**. There are no accounts, ads or telemetry.

Settings, status files and diagnostic logs are stored locally on your PC (`C:\ProgramData\AmLichVN`, `%LOCALAPPDATA%\AmLichVN` and your Windows user registry; the Store version uses its own package data folder). They never leave your PC. Uninstalling removes settings and status files; for the GitHub version the small log folder `%LOCALAPPDATA%\AmLichVN` is kept for you to delete.

The Microsoft Store version makes no network connections; updates are delivered by the Microsoft Store. The GitHub version only contacts `github.com` to check for and download updates, sending no cookies or credentials.

To display lunar dates, the app attaches a component to the Windows taskbar calendar process on your own PC. It only reads and displays dates.

Contact: https://github.com/longho-lhu/vietnamese-lunar-calendar-for-windows/issues
