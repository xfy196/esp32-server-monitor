#pragma once

// ==========================================================================
//  硬件与行为配置
// ==========================================================================

// ---------- OLED / I2C ----------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_I2C_ADDR  0x3C      // 少数模块是 0x3D，屏幕不亮时先试这个

// ---------- 引脚（按芯片型号自动选择）----------
//
//  ESP32-C3 SuperMini      ESP32 DevKit V1 (WROOM-32)
//  ------------------      ------------------------
//  SDA = IO4               SDA = GPIO21
//  SCL = IO5               SCL = GPIO22
//  BOOT 键 = IO9           BOOT 键 = GPIO0
//
// C3 的坑：GPIO8 接了板载 LED、GPIO9 是 BOOT 键，两个都是 strapping 脚
//（复位时必须为高），偏偏 Arduino 的 Wire 默认就用 8/9，所以必须显式重映射。
// 别把这里改回 8/9，也别接到 GPIO20/21（那是串口）。
#if defined(ARDUINO_ESP32C3_DEV) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ESP32C3)
  #define BOARD_IS_C3    1
  #define PIN_I2C_SDA    4
  #define PIN_I2C_SCL    5
  #define PIN_BUTTON     9
#else
  #define BOARD_IS_C3    0
  #define PIN_I2C_SDA    21
  #define PIN_I2C_SCL    22
  #define PIN_BUTTON     0
#endif

// ---------- 按键 ----------
// 两块板都用板载 BOOT 键，不需要额外接线。
// 注意：开机瞬间按住 BOOT 会进入下载模式，正常使用没有影响。
#define ENABLE_BUTTON  1

// ---------- 时序 ----------
#define WIFI_CONNECT_TIMEOUT_MS 20000   // 开机等 WiFi 的最长时间
#define WIFI_RETRY_MS           10000   // 掉线后重连间隔
#define HTTP_TIMEOUT_MS         2500    // 单次请求超时
#define FETCH_INTERVAL_MS       5000    // 拉取服务器指标间隔
#define RENDER_INTERVAL_MS      400     // 屏幕刷新间隔（仪表条动画）
#define PAGE_INTERVAL_MS        7000    // 自动翻页间隔
#define STALE_AFTER_MS          15000   // 超过这个时间没新数据就显示离线

// ---------- 页面 ----------
#define PAGE_COUNT 3
