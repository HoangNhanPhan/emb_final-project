# ESP32 Peripheral Test Rig — AHT20 + LCD I2C + Quạt PWM 4 dây (bản rút gọn)

Project PlatformIO độc lập, **không còn menu test 1-7**. Chạy tự động ngay sau khi cấp nguồn — không cần gõ lệnh gì.

## Hoạt động

Mỗi 1 giây, thiết bị tự động:
1. Đọc nhiệt độ + độ ẩm từ AHT20.
2. Điều khiển quạt theo bậc thang nhiệt độ đơn giản (`FAN_THRESHOLD_*` trong `test_config.h`) — chỉ để xác nhận tương tác phần cứng, **không phải** logic PID của firmware chính.
3. Đọc số xung Tach, tính RPM, lọc nhiễu bằng EMA (`EMA_ALPHA`) + loại giá trị vượt ngưỡng vật lý (`MAX_PHYSICAL_RPM`).
4. Hiển thị lên LCD 16x2:
   - Hàng 0: `T:<nhiệt độ>C H:<độ ẩm>%`
   - Hàng 1: `F:<công suất quạt>% R:<RPM>`
5. Log cùng dữ liệu ra Serial (115200 baud) để đối chiếu.

## Trước khi build

Đấu nối phần cứng theo bảng pin trong `include/test_config.h` — đã xác nhận `LCD_I2C_ADDR = 0x27` và `PIN_FAN_TACH = 33` (GPIO33, dùng pull-up nội, không cần điện trở ngoài).

## Build & Upload

```bash
pio run -t upload
pio device monitor -b 115200
```

## Tùy chỉnh nhanh

Tất cả hằng số điều khiển nằm trong `test_config.h`:
- `FAN_THRESHOLD_HIGH_C` / `FAN_THRESHOLD_MID_C` — ngưỡng nhiệt độ đổi công suất quạt.
- `FAN_DUTY_HIGH_PCT` / `FAN_DUTY_MID_PCT` / `FAN_DUTY_OFF_PCT` — công suất quạt tương ứng mỗi ngưỡng.
- `EMA_ALPHA` — hệ số lọc RPM (càng nhỏ càng mượt nhưng phản hồi chậm hơn).
- `FAN_TACH_PULSES_PER_REV` — kiểm tra lại theo datasheet quạt cụ thể nếu RPM tính ra sai lệch gấp đôi/một nửa.

## Lưu ý

- `PIN_FAN_TACH = 33` khác với `config.h` của firmware chính (đang dùng GPIO34) — nếu muốn khớp pin 100% để tái sử dụng dây nối, xem lại đề xuất đồng bộ pin đã trao đổi trước đó cho firmware chính.
- Code dùng API LEDC cổ điển (`ledcSetup`/`ledcAttachPin`/`ledcWrite`) — nếu PlatformIO tự cài Arduino-ESP32 core bản mới (3.x) gây lỗi biên dịch, ghim `platform = espressif32@6.5.0` trong `platformio.ini`.
