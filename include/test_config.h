#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#include <stdint.h>

// ============================================================================
// Pin definitions — dung chung voi mach that (co the doi lai PIN_FAN_TACH ve 34
// va bo INPUT_PULLUP neu muon khop 100% voi config.h cua firmware chinh).
// ============================================================================
#define PIN_I2C_SDA         21      // I2C data - dung chung cho AHT20 + LCD
#define PIN_I2C_SCL         22      // I2C clock
#define PIN_FAN_PWM         25      // LEDC PWM output -> chan PWM cua quat 4 day
#define PIN_FAN_TACH         26      // Fan tachometer input. GPIO26 la an toan hon cho ngInterrupt
                                      // (dung INPUT_PULLUP trong code, KHONG can dien tro ngoai)

// --- PWM Manual Control Switches (4-position toggle or DIP) ---
#define PIN_FAN_SW_0   14   // Switch 1: 0% PWM
#define PIN_FAN_SW_33  27   // Switch 2: 33% PWM (gan 33%)
#define PIN_FAN_SW_66  32   // Switch 3: 66% PWM (gan 66%)
#define PIN_FAN_SW_100 33   // Switch 4: 100% PWM

#define SENSOR_I2C_FREQ_HZ  100000UL   // 100 kHz standard mode
#define SENSOR_I2C_ADDR_AHT20  0x38    // dia chi chuan AHT20 (Adafruit AHTX0 tu bat)

// LCD I2C — DIA CHI da xac nhan qua I2C scanner truoc do
#define LCD_I2C_ADDR        0x27       // pho bien: 0x27 (PCF8574) hoac 0x3F (PCF8574A)
#define LCD_COLS            16
#define LCD_ROWS            2

// LEDC — tan so toi da = 80MHz / (2^resolution). 12-bit -> toi da ~19531Hz.
// 5 kHz la muc thap van du lot de quat 4-day hoat dong tot va it am thanh nhieu hon.
#define LEDC_FAN_CHANNEL    0
#define LEDC_FAN_RESOLUTION 12         // 12-bit => 0..4095
#define LEDC_FAN_FREQ_HZ    5000      // 5 kHz (thich hop hon voi nhieu loai quat)

// Tach: hau het quat 4-day PC/CPU cho 2 xung / vong quay (tra datasheet neu co)
#define FAN_TACH_PULSES_PER_REV  2

// RPM filtering constants
#define MAX_PHYSICAL_RPM      20000   // Physical upper limit for 4-pin fan — loai gia tri doc nhieu bat thuong
#define EMA_ALPHA             0.1f    // He so loc EMA (cang nho cang muot nhung phan hoi cham hon)

// Nguong dieu khien quat theo nhiet do — bac thang 3 muc (KHONG phai PID):
//   T < 25C                -> 0%
//   25C <= T < 27C         -> 0%   (vung tre)
//   27C <= T <= 30C        -> 50%
//   T > 30C                -> 100%
#define FAN_THRESHOLD_LOW_C    25.0f   // duoi muc nay -> tat quat
#define FAN_THRESHOLD_MID_C    27.0f   // >= muc nay -> 50%
#define FAN_THRESHOLD_HIGH_C   30.0f   // > muc nay  -> 100%
#define FAN_DUTY_HIGH_PCT      100
#define FAN_DUTY_MID_PCT       50
#define FAN_DUTY_OFF_PCT       0

#endif // TEST_CONFIG_H
