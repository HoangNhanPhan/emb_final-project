// ============================================================================
//  ESP32 Peripheral Test Rig — AHT20 + LCD I2C + Quat PWM 4 day
//  BAN RUT GON: khong con menu test 1-7. Chay tu dong ngay sau khi cap nguon,
//  hien thi lien tuc Nhiet do / Do am / Cong suat quat / Toc do quat (RPM)
//  len LCD 16x2 va log qua Serial moi 1 giay.
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <LiquidCrystal_I2C.h>
#include "test_config.h"

// ----------------------------------------------------------------------------
// Global objects
// ----------------------------------------------------------------------------
Adafruit_AHTX0 aht;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

bool ahtReady = false;

volatile uint32_t tachPulseCount = 0;

void IRAM_ATTR tachISR() {
    tachPulseCount++;
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\nESP32 Peripheral Test Rig (rut gon) - khoi dong..."));

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, SENSOR_I2C_FREQ_HZ);

    ahtReady = aht.begin();
    Serial.println(ahtReady ? F("AHT20: OK") : F("AHT20: KHONG TIM THAY (kiem tra day/dia chi 0x38)"));

    lcd.init();
    lcd.backlight();

    pinMode(PIN_FAN_TACH, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_FAN_TACH), tachISR, FALLING);

    // --- Khoi tao cac nut switch dieu khien thu cong PWM ---
    pinMode(PIN_FAN_SW_0,   INPUT_PULLUP);
    pinMode(PIN_FAN_SW_33,  INPUT_PULLUP);
    pinMode(PIN_FAN_SW_66,  INPUT_PULLUP);
    pinMode(PIN_FAN_SW_100, INPUT_PULLUP);

    ledcSetup(LEDC_FAN_CHANNEL, LEDC_FAN_FREQ_HZ, LEDC_FAN_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, LEDC_FAN_CHANNEL);
    ledcWrite(LEDC_FAN_CHANNEL, 0);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(ahtReady ? "Dang khoi dong..." : "AHT20 LOI!");
    delay(800);
}

// ----------------------------------------------------------------------------
// Loop — cap nhat moi 200ms: doc cam bien, doc switch, dieu khien quat, doc tach, hien thi
// ----------------------------------------------------------------------------
void loop() {
    static uint32_t lastUpdate = 0;
    static float emaRpm = 0.0f;

    if (millis() - lastUpdate < 200) {   // <-- cap nhat moi 200ms (5Hz)
        return;
    }
    lastUpdate = millis();

    // --- Doc AHT20 ---
    // Neu doc that bai, giu nguyen gia tri hop le gan nhat (tranh 0.00C lam
    // logic dieu khien quat nhay sai).
    static float temperature = 0.0f;
    static float humidity = 0.0f;
    bool sensorValid = false;
    if (ahtReady) {
        sensors_event_t humEvt, tempEvt;
        if (aht.getEvent(&humEvt, &tempEvt) && tempEvt.temperature > -50.0f) {
            temperature = tempEvt.temperature;
            humidity = humEvt.relative_humidity;
            sensorValid = true;
        }
    }

    // --- Doc 4 switch dieu khien thu cong PWM ---
    // Moi switch bat khi keo xuong GND (nguoc loi, dung INPUT_PULLUP)
    int manualFanPct = -1;   // -1 = khong co switch nao bat -> tu dong
    if (digitalRead(PIN_FAN_SW_0)   == LOW) { manualFanPct =   0; }
    else if (digitalRead(PIN_FAN_SW_33) == LOW) { manualFanPct =  33; }
    else if (digitalRead(PIN_FAN_SW_66) == LOW) { manualFanPct =  66; }
    else if (digitalRead(PIN_FAN_SW_100)== LOW) { manualFanPct = 100; }

    // --- Dieu khien quat ---
    int fanPct;
    if (manualFanPct >= 0) {
        // Che do thu cong: switch bat -> dung muc PWM tuong ung
        fanPct = manualFanPct;
    } else if (ahtReady && sensorValid) {
        // Che do tu dong: dieu khien theo nhiet do (bac thang 3 muc)
        //   T < 25C           -> 0%
        //   25C <= T < 27C    -> 0%   (vung tre)
        //   27C <= T <= 30C   -> 50%
        //   T > 30C           -> 100%
        if (temperature > FAN_THRESHOLD_HIGH_C) {
            fanPct = FAN_DUTY_HIGH_PCT;
        } else if (temperature >= FAN_THRESHOLD_MID_C) {
            fanPct = FAN_DUTY_MID_PCT;
        } else {
            fanPct = FAN_DUTY_OFF_PCT;   // duoi 30C (bao gom ca vung 27..30C) -> tat
        }
    } else {
        // Khong co switch va khong doc duoc cam bien -> tat quat de an toan
        fanPct = FAN_DUTY_OFF_PCT;
    }

    const uint32_t maxDutyRaw = (1UL << LEDC_FAN_RESOLUTION) - 1;  // 4095 voi 12-bit
    ledcWrite(LEDC_FAN_CHANNEL, (maxDutyRaw * fanPct) / 100);
    Serial.printf("DEBUG: fanPct=%d, duty=%u\n", fanPct, (maxDutyRaw * fanPct) / 100);

    // --- Doc Tach, tinh RPM, loc nhieu ---
    noInterrupts();
    uint32_t pulses = tachPulseCount;
    tachPulseCount = 0;
    interrupts();

    float rpm = ((float)pulses / FAN_TACH_PULSES_PER_REV) * 60.0f;
    if (rpm > MAX_PHYSICAL_RPM) {
        rpm = 0.0f;   // loai gia tri doc nhieu vuot qua gioi han vat ly
    }
    emaRpm = EMA_ALPHA * rpm + (1.0f - EMA_ALPHA) * emaRpm;

    // --- Hien thi LCD 16x2 ---
    lcd.setCursor(0, 0);
    if (ahtReady) {
        lcd.print("T:");
        lcd.print(temperature, 1);
        lcd.print("C H:");
        lcd.print(humidity, 0);
        lcd.print("%  ");
    } else {
        lcd.print("AHT20 LOI!      ");
    }

    lcd.setCursor(0, 1);
    lcd.print("F:");
    lcd.print(fanPct);
    lcd.print("% R:");
    lcd.print((int)emaRpm);
    lcd.print("    ");   // xoa ky tu thua neu so giam chu so

    // --- Log Serial de doi chieu ---
    Serial.printf("Temp=%.2fC Hum=%.2f%% Fan=%d%% RPM=%.0f\n",
                  temperature, humidity, fanPct, emaRpm);
}