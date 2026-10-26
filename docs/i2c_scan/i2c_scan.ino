/*
 * I2C 地址扫描 —— 屏幕不亮时先用它确认 OLED 是否被识别
 *
 * 用法：在 Arduino IDE 里打开本文件（文件夹名 i2c_scan 需与 .ino 同名），
 *      选好开发板，上传后打开串口监视器（115200）。
 *      正常应看到：found device at 0x3C
 */

#include <Wire.h>

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  Serial.println();
  Serial.println("I2C scan starting...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("nothing found -> check VCC/GND/SDA/SCL");
  }
  Serial.println("done.");
}

void loop() {
  delay(1000);
}
