# PROJECT CONTEXT: HỆ THỐNG ĐIỀU KHIỂN LÀM MÁT THÔNG MINH VÀ GIÁM SÁT TRÊN NỀN TẢNG ESP32 & FREERTOS

## 1. TỔNG QUAN HỆ THỐNG & TRẠNG THÁI HIỆN TẠI

* **Tên đề tài/dự án:** Hệ thống điều khiển làm mát thông minh và giám sát môi trường không gian kín.

* **Vi điều khiển trung tâm:** ESP32 (Dual-core Xtensa LX6, 240 MHz).

* **Framework & Môi trường:** PlatformIO / Antigravity IDE, ESP32 Arduino Core kết hợp FreeRTOS nguyên bản.

* **Mục tiêu phiên làm việc tiếp theo:**

  * Hoàn thiện cấu trúc và nội dung báo cáo kỹ thuật đồ án (Report / Thesis).

  * Xây dựng hệ thống câu hỏi - câu trả lời chuyên sâu phục vụ bảo vệ và phản biện trước hội đồng chấm thi (Defense Q&A).

  * Phân tích cơ sở lý thuyết, mô hình hóa toán học, giải thuật điều khiển và minh chứng kết quả đo đạc thực nghiệm.

* **Phạm vi áp dụng hiện tại:** Hệ thống vận hành độc lập (Standalone Hardware Validation). Các tính năng giao tiếp mạng (Wi-Fi, MQTT Broker, Web Dashboard) tạm thời được cô lập thông qua cờ `#define ENABLE_NETWORK 0` để ưu tiên tính ổn định tuyệt đối của phần cứng cốt lõi và dữ liệu thực nghiệm.

## 2. BẢNG PHÂN BỔ NGOẠI VI & PHẦN CỨNG (HARDWARE PINOUT)

Toàn bộ hệ thống áp dụng nguyên tắc **Common Ground** (chung Mass giữa nguồn 12V cho quạt, nguồn 5V LCD và nguồn 3.3V ESP32).

| Ngoại vi / Linh kiện | Chân ESP32 | Chế độ cấu hình | Đặc tính điện học & Vai trò | 
| ----- | ----- | ----- | ----- | 
| **Quạt 4-dây BLDC (PWM)** | `GPIO 18` | Output (LEDC Ch0) | Xung PWM 25 kHz, độ phân giải 10-bit ($0 - 1023$). Điều tốc động cơ. | 
| **Quạt 4-dây BLDC (TACH)** | `GPIO 19` | `INPUT_PULLUP` | Cực thu hở (Open-Collector), 2 xung/vòng quay cơ học. Đo tốc độ vòng quay. | 
| **Cảm biến AHT20 (SDA)** | `GPIO 21` | I2C Data | Địa chỉ `0x38`, nguồn 3.3V, tốc độ bus 100 kHz. Đo nhiệt độ, độ ẩm. | 
| **Cảm biến AHT20 (SCL)** | `GPIO 22` | I2C Clock | Chung bus I2C với module LCD. | 
| **Màn hình LCD 16x2 + PCF8574** | `GPIO 21 / 22` | I2C Bus | Địa chỉ `0x27`, nguồn nuôi 5V (tín hiệu giao tiếp 3.3V an toàn). | 
| **Biến trở điều khiển (POT)** | `GPIO 34` | ADC1 Input-only | Điện áp $0 - 3.3\text{V}$, độ phân giải 12-bit ($0 - 4095$), suy hao 11 dB. | 
| **Nút Mode (BTN_ACTIVATE)** | `GPIO 23` | `INPUT_PULLUP` | Active-LOW (nhấn về GND, sườn xuống FALLING). Kích hoạt Operator Mode. | 
| **Nút Nguồn (BTN_ONOFF)** | `GPIO 5` | `INPUT_PULLUP` | Active-LOW (nhấn về GND). Bật/tắt cưỡng bức tại chỗ. | 
| **Đèn LED cảnh báo (ALERT)** | `GPIO 2` | Digital Output | Active-HIGH. Sáng khi hệ thống rơi vào trạng thái `EMERGENCY`. | 

## 3. KIẾN TRÚC PHẦN MỀM & XỬ LÝ TÍN HIỆU ĐÃ CHUẨN HÓA

### 3.1. Chuỗi xử lý tín hiệu Biến trở (ADC Signal Pipeline)

Khắc phục triệt để hiện tượng trôi mức và nhiễu trắng trên ADC ESP32:

1. **Lấy mẫu thô:** Đọc ADC1 12-bit ($0 - 4095$).

2. **Bộ lọc số EMA (Exponential Moving Average):**
   

   $$
   \text{EMA}_{k} = \alpha \times \text{ADC}_{k} + (1 - \alpha) \times \text{EMA}_{k-1} \quad (\alpha = 0.18)
   $$

3. **Vùng chết (Deadband):** Chỉ ghi nhận và cập nhật giá trị mới khi biến thiên thỏa mãn:
   

   $$
   \vert{}\Delta\vert{} = \vert{}\text{EMA}_{k} - \text{LastStableADC}\vert{} \ge 20
   $$

4. **Ánh xạ PWM tối ưu:** Sử dụng phép dịch bit loại bỏ 2 bit nhiễu thấp nhất (LSB):
   

   $$
   \text{PWM}_{\text{Duty}} = \text{StableADC} \gg 2 \quad (\text{Thang đo } 10\text{-bit: } 0 - 1023)
   $$

### 3.2. Đo tốc độ vòng quay (TACH & RPM Calculation)

* **Nguyên lý:** Quạt BLDC có 4 cực từ, 1 vòng quay tạo ra 2 lần kích hoạt cảm biến Hall kéo chân xuống GND.

* **Công thức chuẩn theo chu kỳ trích mẫu 1 giây:**
  

  $$
  \text{RPM} = \frac{\text{Số xung trong 1 giây}}{2} \times 60 = \text{Số xung} \times 30
  $$

* **Hạn chế thực tế & Sai số lượng tử (Quantization Error):** Sai số $\pm 1$ xung dẫn đến bước nhảy rời rạc $30\text{ RPM}$ trên LCD.

* **Giải pháp nâng cao cho phản biện:** Chuyển sang kỹ thuật đo chu kỳ (Period Measurement) thông qua phần cứng chuyên dụng `MCPWM Capture` hoặc `PCNT` của ESP32 với độ phân giải $\mu\text{s}$.

### 3.3. Thuật toán dải trễ Hysteresis (`AUTO_MODE`)

Triệt tiêu hoàn toàn hiện tượng rung lắc rơ-le / băm xung chập chờn (chattering) quanh điểm chuyển đổi nhiệt độ:

* $T \ge 30.5^\circ\text{C}$ (Ngưỡng bật): Kích hoạt quạt với mức Duty Cycle khởi điểm an toàn để chống kẹt cơ học (Stall Zone):
  

  $$
  \text{Duty}_{\text{min}} = 256 \quad (25\% \text{ công suất})
  $$

  
  Công suất tăng tuyến tính từ $30.5^\circ\text{C}$ ($\text{Duty} = 256$) đến $40.0^\circ\text{C}$ ($\text{Duty} = 1023 - 100\%$).

* $T \le 29.5^\circ\text{C}$ (Ngưỡng tắt): Tắt hoàn toàn xung điều khiển ($\text{Duty} = 0$).

* $29.5^\circ\text{C} < T < 30.5^\circ\text{C}$: Giữ nguyên trạng thái vận hành trước đó. Trạng thái khởi tạo an toàn ban đầu: `isFanRunning = false`.

### 3.4. Quản lý trạng thái khẩn cấp (`EMERGENCY_MODE`)

* **Điều kiện kích hoạt:** Khi $T \ge 45.0^\circ\text{C}$ hoặc xảy ra lỗi phần cứng nghiêm trọng.

* **Hành vi:** Quạt quay cưỡng bức $100\%$ công suất ($\text{Duty} = 1023$), đèn LED cảnh báo (`GPIO 2`) bật sáng mức HIGH.

* **Thoát trạng thái an toàn:** Hệ thống tự động giải phóng cờ Emergency và trả về `AUTO_MODE` khi nhiệt độ hạ xuống dưới ngưỡng an toàn thực tế:
  

  $$
  T \le 35.0^\circ\text{C}
  $$

  
  (Loại bỏ hoàn toàn bẫy logic cũ bắt buộc hạ về $20.0^\circ\text{C}$ vốn không thể đạt được trong nhiệt độ phòng).

### 3.5. Đồng bộ hóa I2C Mutex & Giám sát Task WDT

* **Bảo vệ toàn vẹn Bus I2C:** Toàn bộ chu trình đọc cảm biến AHT20 (gửi trigger lệnh `0xAC`, duy trì suốt 80ms chờ cảm biến chuyển đổi dữ liệu, đọc chuỗi byte kết quả) được khóa trọn vẹn bằng `xI2CMutex` trước khi nhả cho Task LCD, loại bỏ nguy cơ tranh chấp bus làm treo hệ thống.

* **Watchdog Timer (Task WDT):** Cấu hình thời gian giám sát 5 giây (`esp_task_wdt_init(5, true)`). WDT được gán trực tiếp vào Task điều khiển cốt lõi (`Task_FanControl`). Nếu I2C bị treo hoặc deadlock xảy ra vượt quá 5 giây, hệ thống sẽ tự động kích hoạt reset vi điều khiển.

* **FreeRTOS:**
* **Phân chia Task (Độ ưu tiên & Stack Size khuyến nghị):**
1. `vTaskControl` (Priority 3, Stack 4096 bytes): Thực thi State Machine, tính toán PWM, kiểm tra an toàn nhiệt độ định kỳ mỗi 500ms.
2. `vTaskSensor` (Priority 2, Stack 3072 bytes): Đọc AHT20 chu kỳ mỗi 1 giây, đẩy dữ liệu qua Queue cho Task Control.
3. `vTaskDisplay` (Priority 1, Stack 3072 bytes): Cập nhật thông số nhiệt/ẩm/RPM lên LCD mỗi 200ms.
4. `vTaskNetwork` (Priority 1, Stack 8192 bytes): Quản lý Wi-Fi và WebSockets độc lập. Tuyệt đối không để Task này block hàm `setup()`.
