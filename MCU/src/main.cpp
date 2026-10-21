#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "state_machine.h"
#include "../lib/AHT20_Driver/AHT20.h"
#if ENABLE_NETWORK
#include "../lib/MQTT_Handler/MQTTHandler.h"
#endif
#include "../lib/LCD_Display/LCD_I2C.h"

// Khai báo prototype cho các hàm quản lý hệ thống.
// Các hàm này được định nghĩa bên dưới để thực hiện setup/trạng thái của firmware.
static void initHardware(void);
static void manualTimeoutCallback(TimerHandle_t xTimer);
static void setSystemMode(SystemMode_t newMode);
static void applyModeAction(SystemMode_t mode);
static void handleAutoMode(float currentTemp);

static void Task_SensorRead(void *pvParameters);
static void Task_FanControl(void *pvParameters);
#if ENABLE_NETWORK
static void Task_Network(void *pvParameters);
#endif
static void Task_Display(void *pvParameters);

// Queue dùng để truyền sample mới nhất từ SensorRead sang FanControl.
// Telemetry queue tách riêng để tránh một consumer ăn mất dữ liệu của consumer khác.
static QueueHandle_t sensorQueue = nullptr;
static QueueHandle_t telemetryQueue = nullptr;
static QueueHandle_t cmdQueue = nullptr;
static EventGroupHandle_t sysEvents = nullptr;
static TimerHandle_t manualTimer = nullptr;
static SemaphoreHandle_t xI2CMutex = nullptr;

// Trạng thái hệ thống toàn cục: mode đang hoạt động, mức duty hiện tại và trạng thái network.
static SystemContext_t g_sys = {
    .current_mode = INIT_MODE,
    .fan_duty_pct = 0,
    .network_ready = false,
    .last_temperature_c = 0.0f,
    .last_humidity_pct = 0.0f,
    .last_sensor_valid = false,
    .fan_rpm = 0,
    .target_temp_c = TARGET_TEMP_DEFAULT_C,
};

static volatile uint32_t g_tachPulseCount = 0;
static volatile uint32_t g_lastTachPulseMs = 0;
static volatile uint32_t g_lastTachPulseUs = 0;
static portMUX_TYPE g_operatorButtonMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_operatorButtonPressed = false;
static volatile uint32_t g_operatorButtonLastMs = 0;
static volatile bool g_operatorActivatePressed = false;
static volatile uint32_t g_operatorActivateLastMs = 0;

// Đối tượng MQTT dùng để kết nối WiFi/MQTT và nhận lệnh điều khiển từ xa.
#if ENABLE_NETWORK
static MQTTHandler mqttHandler;
#endif

extern "C" {
// ISR của tín hiệu tachometer quạt: chỉ đếm xung, không báo khẩn cấp trực tiếp.
static void IRAM_ATTR gpio_isr_handler() {
    // Lọc nhiễu: bỏ qua xung đến quá sát xung trước (nhiễu PWM 25kHz ghép vào tach).
    const uint32_t nowUs = micros();
    if ((uint32_t)(nowUs - g_lastTachPulseUs) < TACH_MIN_PULSE_INTERVAL_US) {
        return;
    }
    g_lastTachPulseUs = nowUs;
    g_tachPulseCount++;
    g_lastTachPulseMs = nowUs / 1000UL;
}

static void IRAM_ATTR operatorButton_isr() {
    const uint32_t nowMs = millis();
    portENTER_CRITICAL_ISR(&g_operatorButtonMux);
    if (nowMs - g_operatorButtonLastMs >= 200UL) {
        g_operatorButtonPressed = true;
        g_operatorButtonLastMs = nowMs;
    }
    portEXIT_CRITICAL_ISR(&g_operatorButtonMux);
}

static void IRAM_ATTR operatorActivate_isr() {
    const uint32_t nowMs = millis();
    portENTER_CRITICAL_ISR(&g_operatorButtonMux);
    if (nowMs - g_operatorActivateLastMs >= 200UL) {
        g_operatorActivatePressed = true;
        g_operatorActivateLastMs = nowMs;
    }
    portEXIT_CRITICAL_ISR(&g_operatorButtonMux);
}
}

// setup(): chạy một lần khi boot hệ thống.
// Chức năng: khởi tạo GPIO, queue, timer, event group và tạo tất cả các task RTOS.
void setup() {
    Serial.begin(115200);  // Mở cổng serial để debug gỡ lỗi và xem log runtime.

    initHardware();  // Khởi tạo I2C, PWM quạt và interrupt tachometer.

    // Tạo queue gửi dữ liệu cảm biến mới nhất và lệnh điều khiển manual từ MQTT.
    sensorQueue = xQueueCreate(1, sizeof(SensorData_t));
    telemetryQueue = xQueueCreate(1, sizeof(SensorData_t));
    cmdQueue = xQueueCreate(CMD_QUEUE_LEN, sizeof(ControlCommand_t));
    sysEvents = xEventGroupCreate();
    xI2CMutex = xSemaphoreCreateMutex();

    configASSERT(sensorQueue != nullptr);
    configASSERT(telemetryQueue != nullptr);
    configASSERT(cmdQueue != nullptr);
    configASSERT(sysEvents != nullptr);
    configASSERT(xI2CMutex != nullptr);

    // Timer dùng để tự động quay lại AUTO_MODE nếu không có lệnh manual mới trong thời gian quy định.
    manualTimer = xTimerCreate(
        "operatorTimeout",
        pdMS_TO_TICKS(OPERATOR_TIMEOUT_MS),
        pdFALSE,
        (void *)0,
        manualTimeoutCallback);
    configASSERT(manualTimer != nullptr);

    // Tạo các task FreeRTOS với ưu tiên khác nhau để tối ưu hóa thời gian và độ nhạy của firmware.
    xTaskCreatePinnedToCore(Task_FanControl, "FanControl",
                            TASK_STACK_FAN, nullptr,
                            TASK_PRIO_FAN, nullptr, 1);
    xTaskCreatePinnedToCore(Task_SensorRead, "SensorRead",
                            TASK_STACK_SENSOR, nullptr,
                            TASK_PRIO_SENSOR, nullptr, 0);
#if ENABLE_NETWORK
    xTaskCreatePinnedToCore(Task_Network, "Network",
                            TASK_STACK_NETWORK, nullptr,
                            TASK_PRIO_NETWORK, nullptr, 0);
#endif
    xTaskCreatePinnedToCore(Task_Display, "Display",
                            TASK_STACK_DISPLAY, nullptr,
                            TASK_PRIO_DISPLAY, nullptr, 0);

    // Scheduler của Arduino-ESP32 đã được khởi động sẵn; không gọi hand-start lại.
}

// Toàn bộ logic và watchdog được quản lý bởi các task FreeRTOS riêng.
void loop() {
}

// Khởi tạo phần cứng: I2C, PWM quạt, interrupt tachometer và watchdog.
static void initHardware(void) {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, SENSOR_I2C_FREQ_HZ);

    // Thiết lập kênh PWM quạt 25kHz, 10-bit resolution cho điều khiển độ rộng xung.
    ledcSetup(LEDC_FAN_CHANNEL, LEDC_FAN_FREQ_HZ, LEDC_FAN_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, LEDC_FAN_CHANNEL);
    ledcWrite(LEDC_FAN_CHANNEL, 0);

    // Tachometer và biến trở Operator.
    pinMode(PIN_LED_ALERT, OUTPUT);
    digitalWrite(PIN_LED_ALERT, LOW);
    pinMode(PIN_FAN_TACH, INPUT_PULLUP);
    pinMode(PIN_OPERATOR_TOGGLE, INPUT_PULLUP);
    pinMode(PIN_OPERATOR_ACTIVATE, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_OPERATOR_POT, ADC_11db);
    attachInterrupt(digitalPinToInterrupt(PIN_FAN_TACH), gpio_isr_handler, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_OPERATOR_TOGGLE), operatorButton_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_OPERATOR_ACTIVATE), operatorActivate_isr, FALLING);

    // WDT ESP32 được khởi tạo để phát hiện task treo hoặc nhầm lẫn trong vòng lặp.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);

    g_tachPulseCount = 0;
    g_lastTachPulseMs = millis();
}

// Cập nhật mode hệ thống và gọi applyModeAction để đổi trạng thái hardware tương ứng.
static void setSystemMode(SystemMode_t newMode) {
    if (g_sys.current_mode == newMode) {
        return;  // Nếu đang ở cùng mode thì bỏ qua để tránh lặp lại hành động không cần thiết.
    }

    g_sys.current_mode = newMode;
    applyModeAction(newMode);
}

// Khởi tạo trạng thái ban đầu an toàn cho quạt trong chế độ AUTO
static bool isFanRunning = false;

// Thuật toán điều khiển quạt cho chế độ AUTO_MODE
// Kết hợp dải trễ Hysteresis (29.5°C - 30.5°C) và ánh xạ tuyến tính chống kẹt động cơ (25% - 100% PWM)
static void handleAutoMode(float currentTemp) {
    if (isnan(currentTemp)) {
        return;
    }

    // 1. Cập nhật trạng thái ON/OFF theo dải trễ Hysteresis
    if (currentTemp >= TEMP_ON_THRESH) {
        isFanRunning = true;
    } else if (currentTemp <= TEMP_OFF_THRESH) {
        isFanRunning = false;
    }
    // Nếu nằm trong khoảng (TEMP_OFF_THRESH, TEMP_ON_THRESH), isFanRunning giữ nguyên giá trị trước đó

    // 2. Tính toán duty PWM (10-bit: 0 - 1023)
    uint16_t duty = 0;
    if (!isFanRunning) {
        duty = 0; // Quạt tắt hoàn toàn
    } else {
        // Ánh xạ nhiệt độ từ [30.5°C -> 40.0°C] sang [PWM_MIN_DUTY -> PWM_MAX_DUTY]
        if (currentTemp >= TEMP_MAX_AUTO) {
            duty = PWM_MAX_DUTY;
        } else {
            long mappedDuty = map((long)(currentTemp * 10.0f), 
                                  (long)(TEMP_ON_THRESH * 10.0f), 
                                  (long)(TEMP_MAX_AUTO * 10.0f), 
                                  PWM_MIN_DUTY, 
                                  PWM_MAX_DUTY);
            // Bảo vệ chống underflow: khi nhiệt độ giảm vào vùng trễ (29.5 - 30.5°C) mà quạt vẫn đang chạy,
            // constrain đảm bảo duty không bị tụt xuống dưới PWM_MIN_DUTY gây kẹt động cơ
            duty = (uint16_t)constrain(mappedDuty, (long)PWM_MIN_DUTY, (long)PWM_MAX_DUTY);
        }
    }

    // 3. Xuất PWM và cập nhật trạng thái hệ thống
    ledcWrite(LEDC_FAN_CHANNEL, duty);
    g_sys.fan_duty_pct = (uint8_t)round(((float)duty * 100.0f) / (float)PWM_MAX_DUTY);
}

// Chuyển đổi trạng thái mode thành PWM quạt tương ứng.
static void applyModeAction(SystemMode_t mode) {
    switch (mode) {
        case INIT_MODE:
            digitalWrite(PIN_LED_ALERT, LOW);
            isFanRunning = false;
            ledcWrite(LEDC_FAN_CHANNEL, 0);
            g_sys.fan_duty_pct = 0;
            break;

        case AUTO_MODE:
            digitalWrite(PIN_LED_ALERT, LOW);
            if (g_sys.last_sensor_valid) {
                handleAutoMode(g_sys.last_temperature_c);
            } else {
                ledcWrite(LEDC_FAN_CHANNEL, 0);
                g_sys.fan_duty_pct = 0;
            }
            break;

        case REMOTE_MODE:
        case OPERATOR_MODE:
            digitalWrite(PIN_LED_ALERT, LOW);
            break;

        case EMERGENCY_MODE:
            digitalWrite(PIN_LED_ALERT, HIGH);
            ledcWrite(LEDC_FAN_CHANNEL, (1UL << LEDC_FAN_RESOLUTION) - 1UL);
            g_sys.fan_duty_pct = 100;
            break;

        default:
            break;
    }
}

// Timer callback: khi lệnh manual quá hạn thì báo event để trở về AUTO_MODE.
static void manualTimeoutCallback(TimerHandle_t xTimer) {
    (void)xTimer;
    if (sysEvents != nullptr) {
           xEventGroupSetBits(sysEvents, EVT_OPERATOR_TIMEOUT);
    }
}

// Task đọc sensor: giữ xI2CMutex trong TOÀN BỘ chu trình đo AHT20
// (trigger 0xAC -> chờ ~80ms chuyển đổi -> đọc 6 byte) để LCD không chen vào bus.
static void Task_SensorRead(void *pvParameters) {
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    static AHT20 sensor(Wire);
    bool sensorInitialized = false;

    for (;;) {
        bool sensorUpdated = false;

        if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == pdTRUE) {
            if (!sensorInitialized) {
                sensorInitialized = sensor.begin();
            }
            if (sensorInitialized) {
                sensor.triggerMeasurement();
                vTaskDelay(pdMS_TO_TICKS(85));  // Chờ cảm biến chuyển đổi, vẫn giữ Mutex.
                sensorUpdated = sensor.readData();
            }
            xSemaphoreGive(xI2CMutex);
        }

        if (sensorUpdated) {
            const AHT20_Data_t raw = sensor.getLastResult();
            SensorData_t sample;
            sample.temperature_c = raw.temperature;
            sample.humidity_pct  = raw.humidity;
            sample.timestamp_ms  = raw.timestamp;
            sample.valid         = raw.valid;
            g_sys.last_temperature_c = sample.temperature_c;
            g_sys.last_humidity_pct = sample.humidity_pct;
            g_sys.last_sensor_valid = sample.valid;

            xQueueOverwrite(sensorQueue, &sample);
            if (sample.valid) {
                xQueueOverwrite(telemetryQueue, &sample);
                xEventGroupSetBits(sysEvents, EVT_SENSOR_OK);
                xEventGroupClearBits(sysEvents, EVT_SENSOR_TIMEOUT);

                if (sample.temperature_c > TEMP_CRITICAL_C ||
                    sample.humidity_pct > HUM_CRITICAL_HIGH ||
                    sample.humidity_pct < HUM_CRITICAL_LOW) {
                    xEventGroupSetBits(sysEvents, EVT_EMERGENCY);
                }
            } else {
                xEventGroupSetBits(sysEvents, EVT_SENSOR_TIMEOUT);
            }
        } else {
            g_sys.last_sensor_valid = false;
            xEventGroupSetBits(sysEvents, EVT_SENSOR_TIMEOUT);
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SENSOR_READ_PERIOD_MS));
    }
}

// Task điều khiển quạt dựa trên mode hiện tại và dữ liệu nhiệt độ từ sensor.
static void Task_FanControl(void *pvParameters) {
    (void)pvParameters;
    SensorData_t data;
    ControlCommand_t command;

    static uint32_t lastPIDTime = 0;
    static float targetTemp = TARGET_TEMP_DEFAULT_C;
    static float emaRpm = 0.0f;
    static uint32_t lastRpmMs = millis();
    static uint16_t activityAdc = 0;
    static float emaAdc = 0.0f;
    static uint16_t acceptedOperatorAdc = 0;
    static uint32_t operatorPwmValue = 0;
    static bool operatorAdcInitialized = false;
    static bool operatorEnabled = true;
    g_sys.target_temp_c = targetTemp;

    // Đăng ký task điều khiển chính vào Task WDT (timeout 5s, đặt ở initHardware).
    esp_task_wdt_add(NULL);

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(sysEvents);
        if (bits & EVT_EMERGENCY) {
            setSystemMode(EMERGENCY_MODE);
            g_sys.fan_duty_pct = 100;
        } else if (bits & EVT_OPERATOR_TIMEOUT) {
            setSystemMode(AUTO_MODE);
            operatorAdcInitialized = false;
            xEventGroupClearBits(sysEvents, EVT_OPERATOR_TIMEOUT | EVT_OPERATOR_ACTIVE);
            xTimerStop(manualTimer, 0);
        } else if (g_sys.current_mode == REMOTE_MODE &&
                   (!(bits & EVT_NETWORK_READY) || (bits & EVT_SENSOR_TIMEOUT))) {
            setSystemMode(AUTO_MODE);
        } else if (g_sys.current_mode == INIT_MODE) {
            setSystemMode(AUTO_MODE);
        }

        bool operatorActivatePressed;
        portENTER_CRITICAL(&g_operatorButtonMux);
        operatorActivatePressed = g_operatorActivatePressed;
        g_operatorActivatePressed = false;
        portEXIT_CRITICAL(&g_operatorButtonMux);
        if (operatorActivatePressed &&
            (g_sys.current_mode == AUTO_MODE || g_sys.current_mode == REMOTE_MODE)) {
            setSystemMode(OPERATOR_MODE);
            operatorEnabled = true;
            operatorAdcInitialized = false;
            xEventGroupClearBits(sysEvents, EVT_OPERATOR_TIMEOUT);
            xEventGroupSetBits(sysEvents, EVT_OPERATOR_ACTIVE);
            xTimerReset(manualTimer, portMAX_DELAY);
            xTimerStart(manualTimer, portMAX_DELAY);
        }

        if (g_sys.current_mode == EMERGENCY_MODE) {
            while (xQueueReceive(cmdQueue, &command, 0) == pdTRUE) {
            }
        } else if (xQueueReceive(cmdQueue, &command, 0) == pdTRUE) {
            switch ((CommandType_t)command.type) {
                case COMMAND_SET_REMOTE_DUTY:
                    if (g_sys.current_mode == AUTO_MODE || g_sys.current_mode == REMOTE_MODE) {
                        setSystemMode(REMOTE_MODE);
                        g_sys.fan_duty_pct = (uint8_t)constrain(command.duty_pct, 0, 100);
                        operatorAdcInitialized = false;
                        xTimerStop(manualTimer, 0);
                    }
                    break;
                case COMMAND_ACTIVATE_OPERATOR:
                    if (g_sys.current_mode == AUTO_MODE || g_sys.current_mode == REMOTE_MODE) {
                        setSystemMode(OPERATOR_MODE);
                        operatorEnabled = true;
                        operatorAdcInitialized = false;
                        xEventGroupClearBits(sysEvents, EVT_OPERATOR_TIMEOUT);
                        xEventGroupSetBits(sysEvents, EVT_OPERATOR_ACTIVE);
                        xTimerReset(manualTimer, portMAX_DELAY);
                        xTimerStart(manualTimer, portMAX_DELAY);
                    }
                    break;
                case COMMAND_RETURN_AUTO:
                    setSystemMode(AUTO_MODE);
                    operatorAdcInitialized = false;
                    xTimerStop(manualTimer, 0);
                    xEventGroupClearBits(sysEvents, EVT_OPERATOR_ACTIVE | EVT_OPERATOR_TIMEOUT);
                    break;
                case COMMAND_OPERATOR_ON:
                case COMMAND_OPERATOR_OFF:
                    if (g_sys.current_mode == OPERATOR_MODE) {
                        operatorEnabled = command.type == COMMAND_OPERATOR_ON;
                        xTimerReset(manualTimer, portMAX_DELAY);
                        xTimerStart(manualTimer, portMAX_DELAY);
                    }
                    break;
            }
        }

        bool operatorButtonPressed;
        portENTER_CRITICAL(&g_operatorButtonMux);
        operatorButtonPressed = g_operatorButtonPressed;
        g_operatorButtonPressed = false;
        portEXIT_CRITICAL(&g_operatorButtonMux);
        if (g_sys.current_mode == OPERATOR_MODE && operatorButtonPressed) {
            operatorEnabled = !operatorEnabled;
            xTimerReset(manualTimer, portMAX_DELAY);
            xTimerStart(manualTimer, portMAX_DELAY);
        }

        const uint32_t nowMs = millis();
        const uint32_t lastPulseMs = (uint32_t)(g_lastTachPulseUs / 1000UL);
        if (g_sys.current_mode != INIT_MODE &&
            g_sys.fan_duty_pct > FAN_STALL_DUTY_PCT &&
            (nowMs - lastPulseMs) > 2000UL) {
            setSystemMode(EMERGENCY_MODE);
            g_sys.fan_duty_pct = 100;
            xEventGroupSetBits(sysEvents, EVT_EMERGENCY);
        }

        if (g_sys.current_mode == EMERGENCY_MODE) {
            g_sys.fan_duty_pct = 100;
            // Thoát khi nhiệt độ đã hạ xuống ngưỡng an toàn và độ ẩm trong giới hạn.
            if (g_sys.last_sensor_valid && g_sys.last_temperature_c <= TEMP_EMERGENCY_EXIT_C &&
                g_sys.last_humidity_pct <= HUM_CRITICAL_HIGH &&
                g_sys.last_humidity_pct >= HUM_CRITICAL_LOW) {
                xEventGroupClearBits(sysEvents, EVT_EMERGENCY);
                setSystemMode(AUTO_MODE);  // applyModeAction tắt LED và áp dụng lại AUTO.
            }
        }

        noInterrupts();
        const uint32_t pulses = g_tachPulseCount;
        g_tachPulseCount = 0;
        interrupts();
        const uint32_t rpmNowMs = millis();
        const uint32_t elapsedMs = rpmNowMs - lastRpmMs;
        lastRpmMs = rpmNowMs;
        const float rpm = elapsedMs == 0 ? emaRpm
            : (pulses * 60000.0f) / (FAN_TACH_PULSES_PER_REV * (float)elapsedMs);
        // Mẫu vượt giới hạn vật lý là nhiễu: bỏ qua thay vì ép về 0 (làm RPM tụt).
        if (rpm <= MAX_PHYSICAL_RPM) {
            emaRpm = EMA_ALPHA * rpm + (1.0f - EMA_ALPHA) * emaRpm;
        }
        g_sys.fan_rpm = (uint16_t)emaRpm;

        if (xQueueReceive(sensorQueue, &data, 0) == pdTRUE && data.valid) {
            if (millis() - lastPIDTime >= 200 && g_sys.current_mode == AUTO_MODE) {
                lastPIDTime = millis();
                handleAutoMode(data.temperature_c);
            }
        }

        const uint32_t pwmMax = (1UL << LEDC_FAN_RESOLUTION) - 1UL;
        if (g_sys.current_mode == OPERATOR_MODE) {
            const uint16_t adcRaw = (uint16_t)constrain(
                analogRead(PIN_OPERATOR_POT), 0, (int)OPERATOR_ADC_MAX);
            // Lọc EMA để triệt gai nhiễu ADC.
            if (!operatorAdcInitialized) {
                emaAdc = (float)adcRaw;
            } else {
                emaAdc = OPERATOR_ADC_EMA_ALPHA * (float)adcRaw +
                         (1.0f - OPERATOR_ADC_EMA_ALPHA) * emaAdc;
            }
            const uint16_t adcValue = (uint16_t)emaAdc;
            const uint16_t adcDelta = adcValue > acceptedOperatorAdc
                ? adcValue - acceptedOperatorAdc
                : acceptedOperatorAdc - adcValue;
            if (!operatorAdcInitialized || adcDelta >= OPERATOR_ADC_DEADBAND) {
                acceptedOperatorAdc = adcValue;
                operatorPwmValue = (uint32_t)(adcValue >> 2);  // 12-bit -> 10-bit
                if (operatorPwmValue > pwmMax) operatorPwmValue = pwmMax;
                if (!operatorAdcInitialized) {
                    activityAdc = adcValue;
                }
                operatorAdcInitialized = true;
            }
            // Chỉ gia hạn timeout khi người dùng thực sự xoay biến trở (vượt ngưỡng lớn),
            // tránh nhiễu ADC liên tục reset timer.
            const uint16_t activityDelta = adcValue > activityAdc
                ? adcValue - activityAdc : activityAdc - adcValue;
            if (activityDelta >= OPERATOR_ACTIVITY_THRESHOLD) {
                activityAdc = adcValue;
                xTimerReset(manualTimer, portMAX_DELAY);
                xTimerStart(manualTimer, portMAX_DELAY);
            }
            uint32_t pwmValue = operatorEnabled ? operatorPwmValue : 0;
            g_sys.fan_duty_pct = (uint8_t)((pwmValue * 100UL) / pwmMax);
            ledcWrite(LEDC_FAN_CHANNEL, pwmValue);
        } else if (g_sys.current_mode != AUTO_MODE) {
            const uint8_t duty = g_sys.fan_duty_pct;
            uint32_t pwmValue = duty == 0
                ? 0
                : (uint32_t)map(duty, 0, 100, 0, (int)pwmMax);
            ledcWrite(LEDC_FAN_CHANNEL, pwmValue);
        }

        esp_task_wdt_reset();  // Nuôi WDT ở cuối chu kỳ; kẹt >5s sẽ tự reset ESP32.
        vTaskDelay(pdMS_TO_TICKS(FAN_UPDATE_PERIOD_MS));
    }
}

// Task network: duy trì kết nối WiFi/MQTT, nhận lệnh manual và publish telemetry.
#if ENABLE_NETWORK
static void Task_Network(void *pvParameters) {
    (void)pvParameters;
    static bool mqttInitialized = false;
    static SensorData_t lastTelemetryData;
    static uint32_t lastTelemetryTime = 0;

    if (!mqttInitialized) {
        mqttInitialized = mqttHandler.begin();
        if (!mqttInitialized) {
            Serial.println("Failed to initialize MQTT handler");
        }
    }

    for (;;) {
        mqttHandler.update();

        bool networkReady = mqttHandler.isConnected();
        if (networkReady != g_sys.network_ready) {
            g_sys.network_ready = networkReady;
            if (networkReady) {
                xEventGroupSetBits(sysEvents, EVT_NETWORK_READY);
                Serial.println("Network ready - MQTT connected");
            } else {
                xEventGroupClearBits(sysEvents, EVT_NETWORK_READY);
                Serial.println("Network not ready - MQTT disconnected");
            }
        }

        // Queue App commands for FanControl.
        if (mqttHandler.hasCommand()) {
            ControlCommand_t cmd = mqttHandler.getCommand();
            if (xQueueSend(cmdQueue, &cmd, 0) == pdTRUE) {
                Serial.printf("App command queued: type=%u duty=%d%%\n",
                              (unsigned)cmd.type, cmd.duty_pct);
            }
        }

        // Publish telemetry theo chu kỳ nếu mạng đã kết nối.
        // Queue telemetry được tách khỏi queue điều khiển quạt để mỗi consumer nhận dữ liệu riêng.
        if (mqttHandler.isConnected() &&
            xQueueReceive(telemetryQueue, &lastTelemetryData, 0) == pdTRUE &&
            lastTelemetryData.valid &&
            millis() - lastTelemetryTime >= MQTT_PUBLISH_PERIOD_MS) {

            const bool alarm = (g_sys.current_mode == EMERGENCY_MODE) ||
                               ((xEventGroupGetBits(sysEvents) & EVT_EMERGENCY) != 0U);
            const bool shutdown = false;

            if (mqttHandler.publishTelemetry(lastTelemetryData,
                                            g_sys.fan_duty_pct,
                                            g_sys.fan_rpm,
                                            g_sys.target_temp_c,
                                            g_sys.current_mode,
                                            alarm,
                                            shutdown)) {
                Serial.printf("Telemetry published: T=%.1fC H=%.1f%% duty=%u mode=%d\n",
                              lastTelemetryData.temperature_c,
                              lastTelemetryData.humidity_pct,
                              g_sys.fan_duty_pct,
                              (int)g_sys.current_mode);
            } else {
                Serial.println("Failed to publish telemetry");
            }
            lastTelemetryTime = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_PERIOD_MS));
    }
}
#endif  // ENABLE_NETWORK

static void Task_Display(void *pvParameters) {
    (void)pvParameters;
    xSemaphoreTake(xI2CMutex, portMAX_DELAY);
    lcdInit(LCD_I2C_ADDR);
    lcdBacklight(true);
    xSemaphoreGive(xI2CMutex);

    uint32_t lastPageChange = millis();
    bool showMode = false;
    char line0[LCD_COLS + 1];
    char line1[LCD_COLS + 1];

    for (;;) {
        if (millis() - lastPageChange >= 2500UL) {
            showMode = !showMode;
            lastPageChange = millis();
        }

        if (g_sys.last_sensor_valid) {
            snprintf(line0, sizeof(line0), "T:%.1fC H:%.0f%%",
                     (double)g_sys.last_temperature_c,
                     (double)g_sys.last_humidity_pct);
        } else {
            snprintf(line0, sizeof(line0), "AHT20 ERROR");
        }

        if (showMode) {
            const char *modeName = "INIT";
            switch (g_sys.current_mode) {
                case AUTO_MODE: modeName = "AUTO"; break;
                case REMOTE_MODE: modeName = "REM"; break;
                case OPERATOR_MODE: modeName = "OPER"; break;
                case EMERGENCY_MODE: modeName = "EMRG"; break;
                case INIT_MODE:
                default: modeName = "INIT"; break;
            }
            snprintf(line1, sizeof(line1), "F:%03u%% %s",
                     (unsigned)g_sys.fan_duty_pct, modeName);
        } else {
            snprintf(line1, sizeof(line1), "F:%03u%% R:%4u",
                     (unsigned)g_sys.fan_duty_pct, (unsigned)g_sys.fan_rpm);
        }

        for (size_t i = strlen(line0); i < LCD_COLS; ++i) {
            line0[i] = ' ';
        }
        line0[LCD_COLS] = '\0';
        for (size_t i = strlen(line1); i < LCD_COLS; ++i) {
            line1[i] = ' ';
        }
        line1[LCD_COLS] = '\0';

        if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            lcdSetCursor(0, 0);
            lcdPrint(line0);
            lcdSetCursor(0, 1);
            lcdPrint(line1);
            xSemaphoreGive(xI2CMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_PERIOD_MS));
    }
}