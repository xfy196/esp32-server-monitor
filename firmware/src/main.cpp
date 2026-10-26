/*
 * ESP32 服务器监控屏
 * ---------------------------------------------------------------
 * 从局域网里的 server-agent 拉取 JSON 指标，显示在 128x64 OLED 上。
 *
 * 页面：1) CPU / 内存   2) 磁盘 / 网速   3) 系统信息
 * 翻页：每 7 秒自动翻，或按板载 BOOT 键手动翻。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "config.h"
#include "secrets.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------------------------------------------------------- 数据模型
struct Metrics {
  bool     valid      = false;
  bool     hasTemp    = false;
  char     host[20]   = "server";
  float    cpu        = 0.0f;
  float    memPct     = 0.0f;
  float    diskPct    = 0.0f;
  float    memUsedGb  = 0.0f;
  float    memTotalGb = 0.0f;
  float    diskUsedGb = 0.0f;
  float    diskTotalGb = 0.0f;
  float    load1      = 0.0f;
  float    load5      = 0.0f;
  float    load15     = 0.0f;
  float    tempC      = 0.0f;
  uint32_t uptime     = 0;
  uint8_t  cores      = 1;
  double   rxBps      = 0.0;
  double   txBps      = 0.0;
  uint32_t latencyMs  = 0;
  uint32_t updatedMs  = 0;
};

static Metrics  m;

// 显示用的平滑值
static float    showCpu  = 0.0f;
static float    showMem  = 0.0f;
static float    showDisk = 0.0f;
static bool     shownOnce = false;

// 网速需要两次采样做差
static uint64_t prevRxBytes = 0;
static uint64_t prevTxBytes = 0;
static uint32_t prevNetMs   = 0;
static bool     haveNetBase = false;

// 状态
static uint8_t  page            = 0;
static uint32_t lastPageSwitch  = 0;
static uint32_t lastFetch       = 0;
static uint32_t lastRender      = 0;
static uint32_t lastWifiAttempt = 0;
static bool     btnWasUp        = true;

// ---------------------------------------------------------------- 小工具
static bool isStale() {
  if (!m.valid) return true;
  return (millis() - m.updatedMs) > STALE_AFTER_MS;
}

static void printRight(const char *text, int y) {
  display.setCursor(SCREEN_WIDTH - (int)strlen(text) * 6, y);
  display.print(text);
}

static void drawBar(int x, int y, int w, int h, float pct) {
  pct = constrain(pct, 0.0f, 100.0f);
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)((w - 2) * pct / 100.0f + 0.5f);
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void fmtUptime(uint32_t secs, char *out, size_t n) {
  uint32_t days  = secs / 86400UL;
  uint32_t hours = (secs % 86400UL) / 3600UL;
  uint32_t mins  = (secs % 3600UL) / 60UL;
  if (days > 0) {
    snprintf(out, n, "%lud %02luh", (unsigned long)days, (unsigned long)hours);
  } else {
    snprintf(out, n, "%02lu:%02lu", (unsigned long)hours, (unsigned long)mins);
  }
}

static void fmtRate(double bps, char *out, size_t n) {
  if (bps >= 1000000.0)      snprintf(out, n, "%.2f MB/s", bps / 1000000.0);
  else if (bps >= 1000.0)    snprintf(out, n, "%.1f kB/s", bps / 1000.0);
  else                       snprintf(out, n, "%.0f B/s", bps);
}

// ---------------------------------------------------------------- 顶部栏
static void drawWifiIcon() {
  long rssi = WiFi.RSSI();
  int level;
  if (rssi > -60)      level = 4;
  else if (rssi > -70) level = 3;
  else if (rssi > -80) level = 2;
  else                 level = 1;

  for (int i = 0; i < 4; i++) {
    int h  = 2 + i * 2;              // 2,4,6,8
    int bx = SCREEN_WIDTH - 12 + i * 4;
    if (i < level) display.fillRect(bx, 8 - h, 3, h, SSD1306_WHITE);
    else           display.drawRect(bx, 8 - h, 3, h, SSD1306_WHITE);
  }
}

static void drawHeader() {
  if (isStale()) {
    // 离线时用反色条替换标题栏，避免和页面内容重叠
    display.fillRect(0, 0, SCREEN_WIDTH, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    char buf[24];
    if (!m.valid) {
      snprintf(buf, sizeof(buf), "WAITING FOR SERVER");
    } else {
      snprintf(buf, sizeof(buf), "OFFLINE %lus AGO",
               (unsigned long)((millis() - m.updatedMs) / 1000UL));
    }
    display.setCursor(0, 0);
    display.print(buf);
    display.setTextColor(SSD1306_WHITE);
    return;
  }

  char name[20];
  strlcpy(name, m.host, sizeof(name));
  display.setCursor(0, 0);
  display.print(name);
  drawWifiIcon();
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);
}

// ---------------------------------------------------------------- 页面 1
static void drawPageCpu() {
  char buf[32];
  drawHeader();

  display.setCursor(0, 12);
  display.print("CPU");
  snprintf(buf, sizeof(buf), "%5.1f%%", showCpu);
  printRight(buf, 12);
  drawBar(0, 21, 128, 6, showCpu);

  snprintf(buf, sizeof(buf), "%ucore  load %.2f", (unsigned)m.cores, m.load1);
  display.setCursor(0, 29);
  display.print(buf);

  display.setCursor(0, 39);
  display.print("MEM");
  snprintf(buf, sizeof(buf), "%5.1f%%", showMem);
  printRight(buf, 39);
  drawBar(0, 48, 128, 6, showMem);

  snprintf(buf, sizeof(buf), "%.1f / %.1f GB", m.memUsedGb, m.memTotalGb);
  display.setCursor(0, 56);
  display.print(buf);
}

// ---------------------------------------------------------------- 页面 2
static void drawPageDisk() {
  char buf[32];
  drawHeader();

  display.setCursor(0, 12);
  display.print("DISK");
  snprintf(buf, sizeof(buf), "%5.1f%%", showDisk);
  printRight(buf, 12);
  drawBar(0, 21, 128, 6, showDisk);

  snprintf(buf, sizeof(buf), "%.1f / %.1f GB", m.diskUsedGb, m.diskTotalGb);
  display.setCursor(0, 29);
  display.print(buf);

  display.setCursor(0, 39);
  display.print("RX");
  fmtRate(m.rxBps, buf, sizeof(buf));
  printRight(buf, 39);

  display.setCursor(0, 49);
  display.print("TX");
  fmtRate(m.txBps, buf, sizeof(buf));
  printRight(buf, 49);

  char up[16];
  fmtUptime(m.uptime, up, sizeof(up));
  display.setCursor(0, 57);
  display.print("UP");
  printRight(up, 57);
}

// ---------------------------------------------------------------- 页面 3
static void drawPageInfo() {
  char buf[32];
  char up[16];
  drawHeader();

  display.setCursor(0, 12);
  display.print("IP");
  printRight(WiFi.localIP().toString().c_str(), 12);

  display.setCursor(0, 21);
  display.print("RSSI");
  snprintf(buf, sizeof(buf), "%ld dBm", (long)WiFi.RSSI());
  printRight(buf, 21);

  fmtUptime(m.uptime, up, sizeof(up));
  display.setCursor(0, 30);
  display.print("UPTIME");
  printRight(up, 30);

  display.setCursor(0, 39);
  display.print("LOAD");
  snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", m.load1, m.load5, m.load15);
  printRight(buf, 39);

  display.setCursor(0, 48);
  display.print("TEMP");
  if (m.hasTemp) snprintf(buf, sizeof(buf), "%.1f C", m.tempC);
  else           snprintf(buf, sizeof(buf), "n/a");
  printRight(buf, 48);

  display.setCursor(0, 57);
  display.print("API");
  snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)m.latencyMs);
  printRight(buf, 57);
}

// ---------------------------------------------------------------- 渲染
static void render() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  switch (page) {
    case 0:  drawPageCpu();  break;
    case 1:  drawPageDisk(); break;
    default: drawPageInfo(); break;
  }
  display.display();
}

static void updateSmoothing() {
  if (!m.valid) return;
  if (!shownOnce) {
    showCpu  = m.cpu;
    showMem  = m.memPct;
    showDisk = m.diskPct;
    shownOnce = true;
  } else {
    showCpu  += (m.cpu     - showCpu)  * 0.25f;
    showMem  += (m.memPct  - showMem)  * 0.25f;
    showDisk += (m.diskPct - showDisk) * 0.25f;
  }
}

// ---------------------------------------------------------------- 拉数据
static bool fetchMetrics() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(SERVER_URL)) {
    Serial.println("[http] begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");
  if (strlen(API_TOKEN) > 0) http.addHeader("X-Token", API_TOKEN);

  uint32_t t0 = millis();
  int code = http.GET();
  uint32_t latency = millis() - t0;

  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] %s -> %d\n", SERVER_URL, code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[json] %s\n", err.c_str());
    return false;
  }

  // --- 基础信息 ---
  strlcpy(m.host, doc["host"] | "server", sizeof(m.host));
  m.cpu    = doc["cpu"]   | 0.0f;
  m.cores  = (uint8_t)(doc["cores"] | 1);
  m.uptime = doc["uptime"] | 0UL;

  m.load1  = doc["load"][0] | 0.0f;
  m.load5  = doc["load"][1] | 0.0f;
  m.load15 = doc["load"][2] | 0.0f;

  // --- 内存（服务端返回 kB）---
  m.memPct     = doc["mem"]["percent"] | 0.0f;
  m.memUsedGb  = (doc["mem"]["used_kb"]  | 0UL) / 1048576.0f;
  m.memTotalGb = (doc["mem"]["total_kb"] | 0UL) / 1048576.0f;

  // --- 磁盘 ---
  m.diskPct     = doc["disk"]["percent"] | 0.0f;
  m.diskUsedGb  = (doc["disk"]["used_kb"]  | 0UL) / 1048576.0f;
  m.diskTotalGb = (doc["disk"]["total_kb"] | 0UL) / 1048576.0f;

  // --- 温度（可能为 null）---
  if (doc["temp_c"].isNull()) {
    m.hasTemp = false;
  } else {
    m.tempC   = doc["temp_c"].as<float>();
    m.hasTemp = true;
  }

  // --- 网速：累计字节数做差 ---
  uint64_t rx = doc["net"]["rx_bytes"].as<uint64_t>();
  uint64_t tx = doc["net"]["tx_bytes"].as<uint64_t>();
  uint32_t now = millis();
  if (haveNetBase && now > prevNetMs && rx >= prevRxBytes && tx >= prevTxBytes) {
    double dt = (now - prevNetMs) / 1000.0;
    if (dt > 0.5) {
      m.rxBps = (double)(rx - prevRxBytes) / dt;
      m.txBps = (double)(tx - prevTxBytes) / dt;
    }
  }
  prevRxBytes = rx;
  prevTxBytes = tx;
  prevNetMs   = now;
  haveNetBase = true;

  m.latencyMs = latency;
  m.updatedMs = millis();
  m.valid     = true;

  Serial.printf("[data] %s cpu=%.1f%% mem=%.1f%% disk=%.1f%% %lums\n",
                m.host, m.cpu, m.memPct, m.diskPct, (unsigned long)latency);
  return true;
}

// ---------------------------------------------------------------- WiFi
static void drawWifiLostScreen() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("WiFi disconnected");
  display.setCursor(0, 12);
  display.print(WIFI_SSID);
  display.setCursor(0, 28);
  display.print("retrying...");
  display.setCursor(0, 44);
  display.print("SSID must be 2.4GHz");
  display.setCursor(0, 56);
  display.print("check src/secrets.h");
  display.display();
}

static bool connectWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("WiFi connecting");
  display.setCursor(0, 12);
  display.print(WIFI_SSID);
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  uint8_t dots = 0;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    dots = (dots + 1) % 4;
    display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
    display.setCursor(0, 24);
    for (uint8_t i = 0; i < dots; i++) display.print(".");
    display.display();
  }

  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok) {
    Serial.printf("[wifi] connected: %s\n", WiFi.localIP().toString().c_str());
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("WiFi OK");
    display.setCursor(0, 16);
    display.print(WiFi.localIP().toString());
    display.display();
    delay(900);
  }
  return ok;
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[monitor] boot");

#if ENABLE_BUTTON
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#endif

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println("[oled] init failed! check SDA/SCL/power and address 0x3C/0x3D");
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Server Monitor");
  display.setCursor(0, 16);
  display.print("booting...");
  display.display();

  connectWiFi();
  fetchMetrics();

  lastWifiAttempt = millis();
  lastPageSwitch  = millis();
  lastFetch       = millis();
}

void loop() {
  uint32_t now = millis();

  // ---- WiFi 掉线：后台重连，屏幕提示 ----
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttempt > WIFI_RETRY_MS) {
      lastWifiAttempt = now;
      Serial.println("[wifi] reconnecting...");
      WiFi.reconnect();
    }
    drawWifiLostScreen();
    delay(100);
    return;
  }

  // ---- 定时拉数据 ----
  if (now - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = now;
    fetchMetrics();
  }

  // ---- 定时重绘 ----
  if (now - lastRender >= RENDER_INTERVAL_MS) {
    lastRender = now;

#if ENABLE_BUTTON
    bool btn = digitalRead(PIN_BUTTON);
    if (btnWasUp && btn == LOW) {
      page = (page + 1) % PAGE_COUNT;
      lastPageSwitch = millis();
      Serial.printf("[ui] page -> %u\n", page);
    }
    btnWasUp = (btn == HIGH);
#endif

    if (millis() - lastPageSwitch >= PAGE_INTERVAL_MS) {
      lastPageSwitch = millis();
      page = (page + 1) % PAGE_COUNT;
    }

    updateSmoothing();
    render();
  }

  delay(10);
}
