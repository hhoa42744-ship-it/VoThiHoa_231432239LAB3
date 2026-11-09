<div style="font-family: 'Times New Roman', Times, serif;">

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
  * Độ ẩm < 30%: Bật 100% công suất trong 30s.
  * Độ ẩm 30% - 35%: Bật 50% công suất trong 20s.
  * Độ ẩm 35% - 40%: Bật 25% công suất trong 10s.
  * Độ ẩm 40% - 60%: Tắt phun sương (đã đạt ngưỡng an toàn).

---

## Cấu trúc kho lưu trữ
+ README.md
+ BÁO CÁO THỰC HÀNH LAB 3.docx
+ sketch_nov7a
+ .gitignore
+ index.html

---

## Liên kết trực quan
Mô phỏng Wokwi: https://wokwi.com/projects/479989925760706561

Web Demo: https://hhoa42744-ship-it.github.io/VoThiHoa_231432239LAB3/

</div>