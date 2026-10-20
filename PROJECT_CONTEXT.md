# PROJECT_CONTEXT

## 1. Sơ đồ ánh xạ phần cứng (Hardware Pinout & Mapping)

* **Vi điều khiển:** ESP32-WROOM-32 (Dual Core 240MHz, 520KB SRAM).
* **I2C Bus 0:**
* `SDA_PIN`: GPIO 21
* `SCL_PIN`: GPIO 22
* `AHT20 Address`: `0x38`
* `PCF8574 LCD Address`: `0x27`
* Tần số Bus: 100 kHz (Standard-mode).


* **Quạt làm mát (4-wire PWM Fan 12V):**
* `FAN_PWM_PIN`: GPIO 18 (LEDC High-Speed Channel 0, Timer 0).
* `FAN_TACH_PIN`: GPIO 19 (Ngắt ngoài / PCNT, cấu hình `INPUT_PULLUP`).


* **Giao diện điều khiển tại chỗ (Operator Inputs):**
* `POT_PIN` (Biến trở): GPIO 34 hoặc GPIO 36 (ADC1_CHANNEL, không bị xung đột với Wi-Fi ADC2).
* `BTN_ACTIVATE_PIN` (Nút chuyển mode): GPIO 23 (Tích cực mức LOW, ngắt `FALLING`, có cơ chế chống dội phím Debounce).
* `BTN_ONOFF_PIN` (Nút bật/tắt quạt ở Operator): GPIO 5 (Tích cực mức LOW).


* **Cảnh báo ngoại vi:**
* `LED_ALERT_PIN`: GPIO 2 (Output điều khiển LED báo động).



---

## 2. Thông số kỹ thuật các ngoại vi (Peripheral Specifications)

* **LEDC (PWM quạt):**
* Tần số đích: `25000 Hz` (25 kHz chuẩn quạt tản nhiệt).
* Độ phân giải Duty: `10-bit` (Dải giá trị `0` đến `1023`).
* Hệ số chia clock thanh ghi: `Div = 80MHz / (25kHz * 1024) = 3.125` (Định dạng Q10.8 là `0x0320`).


* **ADC1 (Biến trở):**
* Độ phân giải: 12-bit (`0` – `4095`), điện áp 0 – 3.3V (Attenuation 11dB).
* Xử lý tín hiệu:
* Áp dụng bộ lọc số EMA (Exponential Moving Average) để khử gai nhiễu.
* Áp dụng vùng chết (Deadband): Ngưỡng biến thiên tối thiểu $\Delta \ge 20$ nấc mới cập nhật giá trị.
* Quy đổi sang PWM: Dịch phải 2 bit (`adc_val >> 2`) để ép dải 12-bit về 10-bit và triệt tiêu 2 bit nhiễu thấp nhất (LSB).




* **Dây TACH (Đo tốc độ vòng quay):**
* Đặc tính: Transistor cực thu hở (Open-Collector), 1 vòng quay quạt sinh ra 2 xung.
* Công thức tính: $\text{RPM} = \frac{\text{Số xung trong 1 giây}}{2} \times 60$.



---

## 3. Kiến trúc FreeRTOS & Phân chia luồng (Concurrency Architecture)

* **Cơ chế bảo vệ tài nguyên (Concurrency & Mutex):**
* Tạo một Mutex tầng ứng dụng: `SemaphoreHandle_t xI2CMutex`.
* Mọi thao tác đọc cảm biến AHT20 (bao gồm cả khoảng chờ 80ms chuyển đổi) và lệnh ghi màn hình LCD qua PCF8574 đều **bắt buộc** phải lấy `xI2CMutex` trước khi gọi bus và nhả ngay sau khi kết thúc.


* **Phân chia Task (Độ ưu tiên & Stack Size khuyến nghị):**
1. `vTaskControl` (Priority 3, Stack 4096 bytes): Thực thi State Machine, tính toán PWM, kiểm tra an toàn nhiệt độ định kỳ mỗi 100ms.
2. `vTaskSensor` (Priority 2, Stack 3072 bytes): Đọc AHT20 chu kỳ mỗi 1 – 2 giây, đẩy dữ liệu qua Queue cho Task Control.
3. `vTaskDisplay` (Priority 1, Stack 3072 bytes): Cập nhật thông số nhiệt/ẩm/RPM lên LCD mỗi 500ms – 1s.
4. `vTaskNetwork` (Priority 1, Stack 8192 bytes): Quản lý Wi-Fi và WebSockets độc lập. Tuyệt đối không để Task này block hàm `setup()`.


* **Giao tiếp ngắt (ISR Rules):**
* Hàm ISR của nút `BTN_ACTIVATE` chỉ ghi nhận thời gian (Software Debounce) và gửi cờ/sự kiện qua FreeRTOS Queue hoặc Task Notification bằng API đuôi `FromISR` (`xQueueSendFromISR`). Tuyệt đối không gọi `delay()`, `Serial.print()` hay xử lý logic nặng trong ISR.



---

## 4. Quy tắc vận hành Máy trạng thái (FSM Business Logic)

* **Chế độ `AUTO` (Mặc định):**
* Dùng thuật toán dải trễ Hysteresis:
* Nhiệt độ $\ge 30.5^\circ\text{C}$ (Ngưỡng bật): Bắt đầu băm xung quạt theo nhiệt độ.
* Nhiệt độ $\le 29.5^\circ\text{C}$ (Ngưỡng tắt): Tắt quạt hoàn toàn (Duty = 0).
* Trong dải $29.5^\circ\text{C} - 30.5^\circ\text{C}$: Giữ nguyên trạng thái vận hành trước đó.




* **Chế độ `OPERATOR`:**
* Kích hoạt khi nhận ngắt từ `BTN_ACTIVATE`.
* Tốc độ quạt được quyết định hoàn toàn bởi biến trở (ADC) và nút `BTN_ONOFF`.
* Timeout an toàn: Nếu người dùng không xoay biến trở hoặc không thao tác trong vòng 30 giây (hoặc nhấn nút nhả lại), hệ thống tự động trả quyền về `AUTO`.


* **Chế độ `REMOTE`:**
* Nhận setpoint/tốc độ từ WebSockets.
* Fallback an toàn: Mất kết nối Wi-Fi hoặc Client ngắt kết nối quá 5 giây -> Tự động chuyển về `AUTO`.


* **Chế độ `EMERGENCY` (Quyền ưu tiên tối thượng):**
* Kích hoạt khi nhiệt độ môi trường vượt ngưỡng nguy hiểm (ví dụ: $\ge 45.0^\circ\text{C}$).
* Ghi đè (Override) toàn bộ các chế độ khác: Ép quạt chạy 100% công suất (Duty = 1023), bật LED cảnh báo.
* Chỉ thoát khỏi `EMERGENCY` khi nhiệt độ hạ về mức an toàn.



---

## 5. Độ tin cậy & Xử lý sự cố (Reliability & Safety)

* **Watchdog Timer:**
* Khai báo Task Watchdog Timer (`esp_task_wdt.h`) với thời gian Timeout là 5 giây.
* Đăng ký và reset WDT định kỳ trong `vTaskControl` để tự khởi động lại ESP32 nếu xảy ra kẹt Bus I2C hoặc deadlock.


* **Môi trường PlatformIO:**
* Baudrate Serial Monitor: `115200`.
* Framework: Arduino-ESP32 Core v3.x.



---