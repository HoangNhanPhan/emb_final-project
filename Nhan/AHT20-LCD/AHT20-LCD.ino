#include <Adafruit_AHTX0.h>
#include <Wire.h> 
#include <LiquidCrystal_I2C.h> // Thêm thư viện LCD I2C

Adafruit_AHTX0 aht;

// Khởi tạo LCD với địa chỉ I2C là 0x27, loại 16 cột 2 hàng
// (Nếu màn hình không hiện chữ, hãy thử đổi 0x27 thành 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2); 

void setup() {
  Serial.begin(115200);
  Serial.println("Khởi động hệ thống...");

  // Định nghĩa chân I2C cho ESP-WROOM-32
  Wire.begin(21, 22); 

  // Khởi động màn hình LCD
  lcd.init();       // Khởi tạo LCD
  lcd.backlight();  // Bật đèn nền
  lcd.setCursor(0, 0);
  lcd.print("System Init...");

  // Khởi động cảm biến AHT20
  if (!aht.begin()) {
    Serial.println("Lỗi: Không tìm thấy AHT20!");
    lcd.clear();
    lcd.print("AHT20 Error!");
    while (1) delay(10);
  }
  
  Serial.println("AHT20 Ready!");
  lcd.clear();
  lcd.print("AHT20 Ready!");
  delay(2000); // Chờ 2 giây để người dùng kịp nhìn thông báo
  lcd.clear();
}

void loop() {
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp); // Đọc dữ liệu mới

  // 1. Hiển thị lên Serial Monitor (để debug)
  Serial.print("Temp: ");
  Serial.print(temp.temperature);
  Serial.println(" C");
  Serial.print("Hum: ");
  Serial.print(humidity.relative_humidity);
  Serial.println(" %");

  // 2. Hiển thị lên màn hình LCD
  // In Nhiệt độ ở Hàng 1
  lcd.setCursor(0, 0); 
  lcd.print("Temp: ");
  lcd.print(temp.temperature, 2); // In giá trị với 1 chữ số thập phân
  lcd.print(" C   ");             // Thêm khoảng trắng để xóa ký tự cũ nếu có

  // In Độ ẩm ở Hàng 2
  lcd.setCursor(0, 1); 
  lcd.print("Hum:  ");
  lcd.print(humidity.relative_humidity, 2);
  lcd.print(" %   ");

  delay(2000); // Tốc độ cập nhật (2 giây/lần)
}

// test git push
// test git push
// test git push