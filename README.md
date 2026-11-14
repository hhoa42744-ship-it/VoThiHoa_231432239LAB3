# Bài Lab 3 - Hệ thống nhúng (Đề 2)

**Sinh viên thực hiện:** Võ Thị Hoa - 231432239

---

## Nội dung đề tài
Giám sát và điều khiển nông nghiệp cho vườn cây công nghiệp (cà phê, hồ tiêu).

---

## Cấu trúc FreeRTOS
Task truyền data (Core 0): Kết nối Wi-Fi/MQTT, lấy dữ liệu từ dataQueue gửi lên Broker.

Task đọc data (Core 1): Đọc cảm biến DHT22, in Serial, đưa dữ liệu vào dataQueue và dataAlert.

Task cảnh báo (Core 1): Đọc dataAlert và tự động điều khiển máy phun sương:

- Độ ẩm < 30%: Bật 100% công suất trong 30s.
- Độ ẩm 30% - 35%: Bật 50% công suất trong 20s.
- Độ ẩm 35% - 40%: Bật 25% công suất trong 10s.
- Độ ẩm 40% - 60%: Tắt phun sương (đã đạt ngưỡng an toàn).

---

## Cấu trúc kho lưu trữ
Cấu trúc kho lưu trữ bao gồm: Báo cáo thực hành, Thư mục mã nguồn lập trình, Tệp giao diện web.

---

## Liên kết trực quan
Mô phỏng Wokwi: [Xem mô phỏng trên Wokwi](https://wokwi.com/projects/479989925760706561)

Web Demo: [Xem trang web demo](https://wokwi.com/projects/480677227669148673)