#pragma once

// ==========================================================================
//  硬件与行为配置
// ==========================================================================

// ---------- OLED / I2C ----------
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_I2C_ADDR  0x3C      // 少数模块是 0x3D，屏幕不亮时先试这个
#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

// ---------- 按键 ----------
// 使用板载 BOOT 键（GPIO0），不需要额外接线。
// 注意：开机瞬间按住 BOOT 会进入下载模式，正常使用没有影响。
#define ENABLE_BUTTON  1
#define PIN_BUTTON     0

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
