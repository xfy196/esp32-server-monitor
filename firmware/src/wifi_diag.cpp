/*
 * WiFi 硬件自检（临时固件）
 * 编译上传：pio run -e wifi_diag -t upload && pio device monitor -e wifi_diag
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_wifi.h>

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("======== WIFI HARDWARE DIAG ========");
  Serial.printf("chip: %s  rev=%d  cores=%d\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("sdk:  %s\n", ESP.getSdkVersion());
  Serial.printf("flash:%u KB  psram:%u\n",
                ESP.getFlashChipSize() / 1024, ESP.getPsramSize());

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("mac STA: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Serial.println("--- WiFi.mode(WIFI_STA) ---");
  bool modeOk = WiFi.mode(WIFI_STA);
  Serial.printf("mode ok: %s\n", modeOk ? "yes" : "NO");
  delay(200);

  wifi_mode_t cur = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode(&cur);
  Serial.printf("esp_wifi_get_mode: err=0x%x mode=%d\n", (unsigned)err, (int)cur);

  Serial.println("--- SoftAP smoke test ---");
  bool apOk = WiFi.softAP("ESP32-WiFiDiag", "12345678");
  Serial.printf("softAP: %s  IP=%s\n",
                apOk ? "OK" : "FAIL",
                WiFi.softAPIP().toString().c_str());
  delay(500);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(200);

  Serial.println("--- scanNetworks (async=false) ---");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n < 0) {
    Serial.printf("SCAN FAIL code=%d  (WiFi radio likely broken or RF not init)\n", n);
  } else if (n == 0) {
    Serial.println("SCAN OK but 0 AP found (radio works; no 2.4G nearby / antenna?)");
  } else {
    Serial.printf("SCAN OK: %d networks\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  %2d  %3ld dBm  ch=%2d  %s  %s\n",
                    i + 1,
                    (long)WiFi.RSSI(i),
                    WiFi.channel(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SEC ",
                    WiFi.SSID(i).c_str());
    }
  }
  WiFi.scanDelete();

  Serial.println("======== DIAG DONE ========");
  Serial.println("PASS if: mode ok + softAP OK + scan >= 0");
  Serial.println("HARD FAIL if: mode/softAP fail or scan < 0");
}

void loop() {
  delay(5000);
  Serial.println("[diag] idle — reset board to re-run");
}
