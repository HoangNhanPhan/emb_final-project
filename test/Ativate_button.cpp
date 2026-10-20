#include <Arduino.h>

#define ACTIVATE_PIN 18 // Đảm bảo đúng chân GPIO bạn đang cắm

SemaphoreHandle_t xActivateSemaphore;
bool isActivated = false; // Biến trạng thái đơn giản để test

// ISR chỉ làm nhiệm vụ cấp Semaphore khi phát hiện sườn xuống (nhấn nút)
void IRAM_ATTR activateISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xActivateSemaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Task xử lý Debounce và logic nút bấm
void vButtonTestTask(void *pvParameters) {
    for (;;) {
        // Task ngủ đông cho đến khi có ngắt xảy ra
        if (xSemaphoreTake(xActivateSemaphore, portMAX_DELAY) == pdTRUE) {
            
            // 1. Debounce phần mềm: Chờ 20ms cho tiếp điểm cơ khí ngừng rung
            vTaskDelay(pdMS_TO_TICKS(20));

            // 2. Xóa sạch các tín hiệu ngắt rác sinh ra trong 20ms dội phím
            while(xSemaphoreTake(xActivateSemaphore, 0) == pdTRUE) {}

            // 3. Xác nhận lại mức logic tĩnh
            if (digitalRead(ACTIVATE_PIN) == LOW) {
                isActivated = !isActivated; // Đảo trạng thái (Toggle)
                Serial.print("[TEST] Trạng thái hiện tại: ");
                Serial.println(isActivated ? "ON (Chiếm quyền)" : "OFF (Nhả quyền)");
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Cấu hình chân kéo trở lên (thả ra là HIGH, nhấn xuống là LOW)
    pinMode(ACTIVATE_PIN, INPUT_PULLUP);

    xActivateSemaphore = xSemaphoreCreateBinary();

    if (xActivateSemaphore != NULL) {
        xTaskCreate(vButtonTestTask, "Button_Task", 2048, NULL, 5, NULL);
        
        // Gắn ngắt ngoài bắt sườn xuống
        attachInterrupt(digitalPinToInterrupt(ACTIVATE_PIN), activateISR, FALLING);
        
        Serial.println("[TEST] Hệ thống kiểm tra Debounce RTOS sẵn sàng!");
    }
}

void loop() {
    // Giải phóng tài nguyên của Task loop mặc định
    vTaskDelete(NULL); 
}//////////////////////////