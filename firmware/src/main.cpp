/*
 * ESP32 服务器监控屏
 * ---------------------------------------------------------------
 * 从局域网里的 server-agent 拉取 JSON 指标，显示在 128x64 OLED 上。
 *
 * 页面：1) CPU / 内存   2) 磁盘 / 网速   3) 系统信息
 * 翻页：每 7 秒自动翻，或短按板载 BOOT 键手动翻。
 * 配网：首次/凭据失效开热点 ESP32-Monitor；长按 BOOT 约 3 秒清除配置。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>
#if __has_include("soc/rtc_cntl_reg.h")
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
#endif

#include "config.h"
#include "secrets.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;
WebServer webServer(80);
DNSServer dnsServer;

/** @brief 运行时 WiFi SSID（NVS / 配网页） */
static char wifiSsid[33];
/** @brief 运行时 WiFi 密码 */
static char wifiPass[65];
/** @brief 运行时服务器地址 */
static char serverUrl[96];
/** @brief 运行时 API Token */
static char apiToken[64];

/** @brief 配网页已提交，请求结束门户 */
static volatile bool portalSaveRequested = false;

/** @brief SoftAP 期间扫描到的附近 WiFi（去重后） */
static const int WIFI_SCAN_MAX = 16;
static char scanSsids[WIFI_SCAN_MAX][33];
static int8_t scanRssi[WIFI_SCAN_MAX];
static bool scanSecure[WIFI_SCAN_MAX];
static int scanCount = 0;

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
static uint32_t btnDownSince    = 0;
static bool     btnLongHandled  = false;

// ---------------------------------------------------------------- 小工具
/**
 * @brief 判断最近一次指标是否过期
 * @return 无数据或超过 STALE_AFTER_MS 返回 true
 */
static bool isStale() {
  if (!m.valid) return true;
  return (millis() - m.updatedMs) > STALE_AFTER_MS;
}

/**
 * @brief 在指定行右对齐打印文本
 * @param text 要显示的字符串
 * @param y 像素行号
 */
static void printRight(const char *text, int y) {
  display.setCursor(SCREEN_WIDTH - (int)strlen(text) * 6, y);
  display.print(text);
}

/**
 * @brief 绘制百分比进度条
 * @param x 左上角 x
 * @param y 左上角 y
 * @param w 宽度
 * @param h 高度
 * @param pct 0–100
 */
static void drawBar(int x, int y, int w, int h, float pct) {
  pct = constrain(pct, 0.0f, 100.0f);
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)((w - 2) * pct / 100.0f + 0.5f);
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

/**
 * @brief 格式化运行时间为可读字符串
 * @param secs 秒数
 * @param out 输出缓冲
 * @param n 缓冲长度
 */
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

/**
 * @brief 格式化字节速率
 * @param bps 字节/秒
 * @param out 输出缓冲
 * @param n 缓冲长度
 */
static void fmtRate(double bps, char *out, size_t n) {
  if (bps >= 1000000.0)      snprintf(out, n, "%.2f MB/s", bps / 1000000.0);
  else if (bps >= 1000.0)    snprintf(out, n, "%.1f kB/s", bps / 1000.0);
  else                       snprintf(out, n, "%.0f B/s", bps);
}

// ---------------------------------------------------------------- 配置持久化
/**
 * @brief 从 NVS 加载 WiFi / 服务器配置，缺省用 secrets.h
 */
static void loadAppConfig() {
  prefs.begin("monitor", true);
  String ssid = prefs.getString("wifi_ssid", WIFI_SSID);
  String pass = prefs.getString("wifi_pass", WIFI_PASSWORD);
  String url  = prefs.getString("server_url", SERVER_URL);
  String tok  = prefs.getString("api_token", API_TOKEN);
  prefs.end();

  if (url.length() == 0) url = SERVER_URL;
  strlcpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, pass.c_str(), sizeof(wifiPass));
  strlcpy(serverUrl, url.c_str(), sizeof(serverUrl));
  strlcpy(apiToken, tok.c_str(), sizeof(apiToken));
  Serial.printf("[cfg] ssid=%s url=%s token=%s\n",
                strlen(wifiSsid) ? wifiSsid : "(empty)",
                serverUrl,
                strlen(apiToken) > 0 ? "(set)" : "(empty)");
}

/**
 * @brief 把 WiFi 与服务器配置写入 NVS
 * @param ssid WiFi SSID
 * @param pass WiFi 密码
 * @param url Server URL
 * @param tok API Token（可为空）
 */
static void saveAppConfig(const char *ssid, const char *pass,
                          const char *url, const char *tok) {
  if (!ssid) ssid = "";
  if (!pass) pass = "";
  if (!url) url = "";
  if (!tok) tok = "";
  prefs.begin("monitor", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.putString("server_url", url);
  prefs.putString("api_token", tok);
  prefs.end();
  strlcpy(wifiSsid, ssid, sizeof(wifiSsid));
  strlcpy(wifiPass, pass, sizeof(wifiPass));
  strlcpy(serverUrl, url, sizeof(serverUrl));
  strlcpy(apiToken, tok, sizeof(apiToken));
  Serial.println("[cfg] saved to NVS");
}

/**
 * @brief 清除 NVS 中的应用配置
 */
static void clearAppConfig() {
  prefs.begin("monitor", false);
  prefs.clear();
  prefs.end();
  wifiSsid[0] = wifiPass[0] = '\0';
}

// ---------------------------------------------------------------- 顶部栏
/**
 * @brief 绘制右上角 WiFi 信号格
 */
static void drawWifiIcon() {
  long rssi = WiFi.RSSI();
  int level;
  if (rssi > -60)      level = 4;
  else if (rssi > -70) level = 3;
  else if (rssi > -80) level = 2;
  else                 level = 1;

  for (int i = 0; i < 4; i++) {
    int h  = 2 + i * 2;
    int bx = SCREEN_WIDTH - 12 + i * 4;
    if (i < level) display.fillRect(bx, 8 - h, 3, h, SSD1306_WHITE);
    else           display.drawRect(bx, 8 - h, 3, h, SSD1306_WHITE);
  }
}

/**
 * @brief 绘制标题栏（主机名或离线提示）
 */
static void drawHeader() {
  if (isStale()) {
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
/**
 * @brief CPU / 内存页
 */
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
/**
 * @brief 磁盘 / 网速页
 */
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
/**
 * @brief 系统信息页
 */
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
/**
 * @brief 按当前页刷新 OLED
 */
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

/**
 * @brief 对 CPU/内存/磁盘百分比做平滑插值
 */
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
/**
 * @brief 向 agent 拉取 JSON 指标并更新 Metrics
 * @return 成功解析返回 true
 */
static bool fetchMetrics() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (strlen(serverUrl) == 0) {
    Serial.println("[http] server url empty — open portal");
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(serverUrl)) {
    Serial.println("[http] begin failed");
    return false;
  }
  http.addHeader("Accept", "application/json");
  if (strlen(apiToken) > 0) http.addHeader("X-Token", apiToken);

  uint32_t t0 = millis();
  int code = http.GET();
  uint32_t latency = millis() - t0;

  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] %s -> %d\n", serverUrl, code);
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

  strlcpy(m.host, doc["host"] | "server", sizeof(m.host));
  m.cpu    = doc["cpu"]   | 0.0f;
  m.cores  = (uint8_t)(doc["cores"] | 1);
  m.uptime = doc["uptime"] | 0UL;

  m.load1  = doc["load"][0] | 0.0f;
  m.load5  = doc["load"][1] | 0.0f;
  m.load15 = doc["load"][2] | 0.0f;

  m.memPct     = doc["mem"]["percent"] | 0.0f;
  m.memUsedGb  = (doc["mem"]["used_kb"]  | 0UL) / 1048576.0f;
  m.memTotalGb = (doc["mem"]["total_kb"] | 0UL) / 1048576.0f;

  m.diskPct     = doc["disk"]["percent"] | 0.0f;
  m.diskUsedGb  = (doc["disk"]["used_kb"]  | 0UL) / 1048576.0f;
  m.diskTotalGb = (doc["disk"]["total_kb"] | 0UL) / 1048576.0f;

  if (doc["temp_c"].isNull()) {
    m.hasTemp = false;
  } else {
    m.tempC   = doc["temp_c"].as<float>();
    m.hasTemp = true;
  }

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

// ---------------------------------------------------------------- WiFi / 配网 UI
/**
 * @brief 掉线提示屏
 */
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
  display.print(WiFi.SSID());
  display.setCursor(0, 28);
  display.print("retrying...");
  display.setCursor(0, 44);
  display.print("SSID must be 2.4GHz");
  display.setCursor(0, 56);
  display.print("hold BOOT = reset");
  display.display();
}

/**
 * @brief 配网门户 OLED 提示
 */
static void drawPortalScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("WiFi setup mode");
  display.setCursor(0, 12);
  display.print(WIFI_AP_NAME);
  display.setCursor(0, 24);
  display.print("pwd ");
  display.print(WIFI_AP_PASSWORD);
  display.setCursor(0, 40);
  display.print("browser:");
  display.setCursor(0, 52);
  display.print("192.168.4.1");
  display.display();
}

/**
 * @brief 清除配置并重启进入配网
 */
static void factoryResetWifi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Clearing config...");
  display.setCursor(0, 16);
  display.print("reboot to portal");
  display.display();

  Serial.println("[wifi] factory reset");
  clearAppConfig();
  WiFi.disconnect(true, true);
  delay(500);
  ESP.restart();
}

/**
 * @brief 降低配网时功耗，减轻 SuperMini 欠压复位
 */
static void prepareApPower() {
#if defined(RTC_CNTL_BROWN_OUT_REG)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.println("[wifi] brownout detector disabled");
#endif
  setCpuFrequencyMhz(80);
  WiFi.setSleep(false);
  // SoftAP 用中等功率，避免 USB 供电瞬间拉垮
  WiFi.setTxPower(WIFI_POWER_11dBm);
}

/**
 * @brief SoftAP 事件日志（手机连上/断开）
 * @param event WiFi 事件
 * @param info 事件详情
 */
static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("[ap] started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("[ap] client JOIN  AID=%u\n", info.wifi_ap_staconnected.aid);
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("[ap] DHCP assigned");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[ap] client LEAVE AID=%u\n",
                    info.wifi_ap_stadisconnected.aid);
      break;
    default:
      break;
  }
}

/**
 * @brief HTML 转义后写入响应（极简）
 * @param s 原始字符串
 */
static String htmlEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += F("&amp;");
    else if (c == '<') o += F("&lt;");
    else if (c == '"') o += F("&quot;");
    else o += c;
  }
  return o;
}

/**
 * @brief 扫描附近 2.4G WiFi，结果写入 scanSsids[]
 * @note SoftAP 期间需 WIFI_AP_STA；STA 保持断开、不自动重连
 */
static void doWifiScan() {
  Serial.println("[wifi] scanning nearby AP...");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Scanning WiFi...");
  display.display();

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(50);

  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  scanCount = 0;
  if (n < 0) {
    Serial.printf("[wifi] scan failed code=%d\n", n);
    drawPortalScreen();
    return;
  }

  for (int i = 0; i < n && scanCount < WIFI_SCAN_MAX; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < scanCount; j++) {
      if (s.equals(scanSsids[j])) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    strlcpy(scanSsids[scanCount], s.c_str(), sizeof(scanSsids[0]));
    scanRssi[scanCount] = (int8_t)WiFi.RSSI(i);
    scanSecure[scanCount] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    scanCount++;
  }
  WiFi.scanDelete();
  Serial.printf("[wifi] scan ok: %d networks\n", scanCount);
  drawPortalScreen();
}

/**
 * @brief 配网首页（含附近 WiFi 列表）
 */
static void handlePortalRoot() {
  String html;
  html.reserve(1600 + (size_t)scanCount * 100);
  html += F("<!DOCTYPE html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>ESP32 Monitor</title>"
            "<style>body{font-family:sans-serif;margin:16px;max-width:420px}"
            "input{width:100%;padding:8px;margin:6px 0 12px;box-sizing:border-box}"
            "button{padding:10px 12px;width:100%;display:block;box-sizing:border-box;"
            "text-align:left;background:#f5f5f5;border:1px solid #ddd;margin:4px 0;border-radius:6px}"
            "button.pri{background:#1a73e8;color:#fff;border:none;text-align:center;margin-top:10px}"
            ".meta{color:#666;font-size:12px;float:right}</style></head><body>");
  html += F("<h3>ESP32 Server Monitor</h3>"
            "<p>Nearby WiFi (2.4G) · <a href='/scan'>Refresh</a></p>");
  if (scanCount <= 0) {
    html += F("<p class=meta>No APs found. Tap Refresh or type SSID below.</p>");
  } else {
    for (int i = 0; i < scanCount; i++) {
      String js = String(scanSsids[i]);
      js.replace("\\", "\\\\");
      js.replace("'", "\\'");
      html += F("<button type=button onclick=\"document.querySelector('[name=ssid]').value='");
      html += js;
      html += F("';document.querySelector('[name=pass]').focus()\">");
      html += htmlEscape(String(scanSsids[i]));
      html += F("<span class=meta>");
      html += String((int)scanRssi[i]);
      html += F("dBm ");
      html += scanSecure[i] ? F("SEC") : F("OPEN");
      html += F("</span></button>");
    }
  }

  html += F("<form method=POST action=/save>"
            "<label>WiFi SSID</label>");
  html += F("<input name=ssid id=ssid required value='");
  html += htmlEscape(String(wifiSsid));
  html += F("'>"
            "<label>WiFi Password</label>"
            "<input name=pass type=password value='");
  html += htmlEscape(String(wifiPass));
  html += F("'>"
            "<label>Server URL</label>"
            "<input name=url required value='");
  html += htmlEscape(String(serverUrl));
  html += F("'>"
            "<label>API Token</label>"
            "<input name=token value='");
  html += htmlEscape(String(apiToken));
  html += F("'>"
            "<button class=pri type=submit>Save & Connect</button></form>"
            "</body></html>");
  webServer.send(200, "text/html", html);
}

/**
 * @brief 重新扫描附近 WiFi 后回到首页
 */
static void handlePortalScan() {
  doWifiScan();
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

/**
 * @brief 处理配网表单提交
 */
static void handlePortalSave() {
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  String url  = webServer.arg("url");
  String tok  = webServer.arg("token");
  ssid.trim();
  url.trim();
  if (ssid.length() == 0 || url.length() == 0) {
    webServer.send(400, "text/plain", "ssid and url required");
    return;
  }
  saveAppConfig(ssid.c_str(), pass.c_str(), url.c_str(), tok.c_str());
  webServer.send(200, "text/html",
                 F("<html><body><h3>Saved</h3>"
                   "<p>Connecting... AP will close.</p></body></html>"));
  portalSaveRequested = true;
  Serial.printf("[portal] saved ssid=%s\n", wifiSsid);
}

/**
 * @brief 强制打开配网页（Captive Portal）
 */
static void handlePortalCaptive() {
  webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  webServer.send(302, "text/plain", "");
}

/**
 * @brief 用已存 SSID 连接路由器
 * @param timeoutMs 最长等待
 * @return 连上返回 true
 */
static bool connectSta(uint32_t timeoutMs) {
  if (strlen(wifiSsid) == 0) return false;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Connecting WiFi");
  display.setCursor(0, 14);
  display.print(wifiSsid);
  display.display();

  setCpuFrequencyMhz(160);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(wifiSsid, wifiPass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
#if ENABLE_BUTTON
    if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("[wifi] BOOT during STA — abort");
      return false;
    }
#endif
  }
  return WiFi.status() == WL_CONNECTED;
}

/**
 * @brief 开纯 SoftAP 配网页（不用 WiFiManager，避免 AP+STA 连不上）
 * @return 配网成功并连上路由器返回 true
 */
static bool openConfigPortal() {
  prepareApPower();
  WiFi.onEvent(onWifiEvent);

  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    portalSaveRequested = false;
    Serial.printf("[wifi] SoftAP portal attempt %u\n", attempt + 1);

    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(300);

    // AP_STA：热点给手机连，同时可扫描附近路由器
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    bool apOk = WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, 4);
    Serial.printf("[wifi] softAP=%s ip=%s ch=%d\n",
                  apOk ? "OK" : "FAIL",
                  WiFi.softAPIP().toString().c_str(),
                  WIFI_AP_CHANNEL);
    if (!apOk) {
      delay(500);
      continue;
    }

    doWifiScan();
    drawPortalScreen();

    dnsServer.start(53, "*", WiFi.softAPIP());
    webServer.on("/", HTTP_GET, handlePortalRoot);
    webServer.on("/scan", HTTP_GET, handlePortalScan);
    webServer.on("/save", HTTP_POST, handlePortalSave);
    webServer.on("/generate_204", HTTP_GET, handlePortalCaptive);
    webServer.on("/hotspot-detect.html", HTTP_GET, handlePortalCaptive);
    webServer.on("/connecttest.txt", HTTP_GET, handlePortalCaptive);
    webServer.onNotFound(handlePortalRoot);
    webServer.begin();

    uint32_t start = millis();
    uint32_t lastLog = 0;
    uint32_t holdFrom = 0;
    while (millis() - start < (uint32_t)WIFI_PORTAL_TIMEOUT_S * 1000UL) {
      dnsServer.processNextRequest();
      webServer.handleClient();

      if (millis() - lastLog > 2000) {
        lastLog = millis();
        Serial.printf("[ap] stations=%u heap=%u\n",
                      WiFi.softAPgetStationNum(), ESP.getFreeHeap());
      }

      if (portalSaveRequested) {
        delay(500);
        webServer.stop();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        delay(200);
        if (connectSta(WIFI_CONNECT_TIMEOUT_MS * 2)) return true;
        Serial.println("[wifi] STA failed after save — reopen portal");
        break;  // 下一轮 attempt 重新开热点
      }

#if ENABLE_BUTTON
      if (digitalRead(PIN_BUTTON) == LOW) {
        if (holdFrom == 0) holdFrom = millis();
        else if (millis() - holdFrom >= CONFIG_RESET_HOLD_MS) {
          factoryResetWifi();
        }
      } else {
        holdFrom = 0;
      }
#endif
      delay(2);
    }

    webServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    if (!portalSaveRequested) {
      Serial.println("[wifi] portal timeout");
      return false;
    }
  }
  return false;
}

/**
 * @brief 连接 WiFi：已存凭据优先，否则开 SoftAP 配网
 * @return 已连上 STA 返回 true
 */
static bool connectWiFi() {
  loadAppConfig();

  bool forcePortal = false;
#if ENABLE_BUTTON
  forcePortal = (digitalRead(PIN_BUTTON) == LOW);
#endif
  if (forcePortal) {
    Serial.println("[wifi] BOOT held — clear + portal");
    clearAppConfig();
    loadAppConfig();
  }

  bool ok = false;
  if (!forcePortal && strlen(wifiSsid) > 0) {
    ok = connectSta(WIFI_CONNECT_TIMEOUT_MS);
  }
  if (!ok) {
    ok = openConfigPortal();
  }

  if (ok) {
    Serial.printf("[wifi] connected: %s (%s)\n",
                  WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("WiFi OK");
    display.setCursor(0, 16);
    display.print(WiFi.SSID());
    display.setCursor(0, 32);
    display.print(WiFi.localIP().toString());
    display.display();
    delay(900);
  } else {
    Serial.println("[wifi] portal timeout / connect failed");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("WiFi failed");
    display.setCursor(0, 16);
    display.print("hold BOOT reset");
    display.display();
  }
  return ok;
}

/**
 * @brief 处理 BOOT：短按翻页，长按清除配网
 */
static void handleButton() {
#if ENABLE_BUTTON
  bool pressed = (digitalRead(PIN_BUTTON) == LOW);

  if (pressed) {
    if (btnWasUp) {
      btnDownSince = millis();
      btnLongHandled = false;
    } else if (!btnLongHandled &&
               (millis() - btnDownSince) >= CONFIG_RESET_HOLD_MS) {
      btnLongHandled = true;
      factoryResetWifi();
    }
  } else if (!btnWasUp && !btnLongHandled) {
    // 短按松开：翻页
    if ((millis() - btnDownSince) < CONFIG_RESET_HOLD_MS) {
      page = (page + 1) % PAGE_COUNT;
      lastPageSwitch = millis();
      Serial.printf("[ui] page -> %u\n", page);
    }
  }

  btnWasUp = !pressed;
#else
  (void)0;
#endif
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[monitor] boot");
  Serial.printf("[monitor] reset_reason=%d\n", (int)esp_reset_reason());

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

  // 任意时刻可长按 BOOT 清配置
  handleButton();

  if (WiFi.status() != WL_CONNECTED) {
    static uint8_t reconnectFails = 0;
    if (now - lastWifiAttempt > WIFI_RETRY_MS) {
      lastWifiAttempt = now;
      Serial.println("[wifi] reconnecting...");
      WiFi.reconnect();
      delay(2500);
      if (WiFi.status() != WL_CONNECTED) {
        reconnectFails++;
        // 连续失败：重新开配网热点，避免「重烧后看不到 ESP32-Monitor」
        if (reconnectFails >= 2) {
          reconnectFails = 0;
          Serial.println("[wifi] reopen config portal");
          if (openConfigPortal() && WiFi.status() == WL_CONNECTED) {
            lastFetch = millis();
            return;
          }
        }
      } else {
        reconnectFails = 0;
      }
    }
    drawWifiLostScreen();
    delay(100);
    return;
  }

  if (now - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = now;
    fetchMetrics();
  }

  if (now - lastRender >= RENDER_INTERVAL_MS) {
    lastRender = now;

    if (millis() - lastPageSwitch >= PAGE_INTERVAL_MS) {
      lastPageSwitch = millis();
      page = (page + 1) % PAGE_COUNT;
    }

    updateSmoothing();
    render();
  }

  delay(10);
}
