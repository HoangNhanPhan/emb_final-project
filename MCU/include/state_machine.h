

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdint.h>

// -----------------------------------------------------------------------------
// Enum định nghĩa các trạng thái hoạt động của hệ thống quạt và sensor.
// Mỗi mode có mức ưu tiên riêng; mode có độ ưu tiên cao hơn sẽ ghi đè lên mode thấp hơn.
// -----------------------------------------------------------------------------
typedef enum {
    INIT_MODE = 0,      // Giai đoạn khởi tạo ban đầu: I2C, PWM, queue, timer và mạng đang setup.
    AUTO_MODE,          // Chế độ mặc định: quạt điều khiển theo nhiệt độ cảm biến và gửi telemetry.
    REMOTE_MODE,        // App điều khiển setpoint quạt qua MQTT.
    OPERATOR_MODE,      // App kích hoạt; biến trở ADC điều khiển PWM trực tiếp.
    EMERGENCY_MODE      // Chế độ khẩn cấp khi vượt ngưỡng nguy hiểm, ưu tiên cao nhất.
} SystemMode_t;

// -----------------------------------------------------------------------------
// Cấu trúc dữ liệu mẫu đo của sensor.
// Đây là dạng trung gian dùng chung cho việc truyền từ task SensorRead sang task FanControl và MQTT.
// -----------------------------------------------------------------------------
typedef struct {
    float temperature_c;    // Nhiệt độ tính bằng độ C, được đọc trực tiếp từ AHT20.
    float humidity_pct;     // Độ ẩm tương đối tính theo %, từ 0 đến 100.
    uint32_t timestamp_ms;  // Thời gian lấy mẫu theo millis(), dùng chấm thời gian và điều kiện publish.
    bool valid;             // Cờ báo dữ liệu này hợp lệ hay bị lỗi trong quá trình đọc I2C.
} SensorData_t;

// -----------------------------------------------------------------------------
// Cấu trúc command điều khiển từ App qua MQTT.
// -----------------------------------------------------------------------------
typedef struct {
    int duty_pct;           // Mức duty quạt được clamp tại runtime và có thể tạm thời âm trong test.
    uint32_t timestamp_ms;  // Thời gian nhận lệnh để tính timeout và ưu tiên trong manual mode.
    uint8_t type;
} ControlCommand_t;

typedef enum {
    COMMAND_SET_REMOTE_DUTY = 0,
    COMMAND_ACTIVATE_OPERATOR,
    COMMAND_RETURN_AUTO,
    COMMAND_OPERATOR_ON,
    COMMAND_OPERATOR_OFF
} CommandType_t;

// -----------------------------------------------------------------------------
// Cấu trúc toàn trạng thái hệ thống hiện tại.
// Các biến này được chia sẻ giữa các task khác nhau nên được khai báo volatile để tránh tối ưu sai.
// -----------------------------------------------------------------------------
typedef struct {
    volatile SystemMode_t current_mode;  // Mode active hiện tại của hệ thống.
    volatile uint8_t     fan_duty_pct;   // Mức duty quạt hiện đang áp dụng, từ 0 đến 100.
    volatile bool        network_ready;  // Cờ báo MQTT đã kết nối hoặc không.
    volatile float       last_temperature_c;
    volatile float       last_humidity_pct;
    volatile bool        last_sensor_valid;
    volatile uint16_t    fan_rpm;
    volatile float       target_temp_c;
} SystemContext_t;

#endif // STATE_MACHINE_H