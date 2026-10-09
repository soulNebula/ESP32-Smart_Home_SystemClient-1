#pragma once

#include <stdbool.h>
#include <stdint.h>

// ======================================================================
// 屏幕：0.96 寸 SSD1306，128x64，I2C
//
// 对应 ESP-IDF 工程的 components/BSP/OLED/oled.c 加 astra UI 那几页。
// ESP-IDF 版是自绘 GUI 库，Arduino 这份用 U8g2 画同样几页：
// 主页 / 设备页 / 传感器页 / 自检页 / 联网页。
// ======================================================================

#define OLED_WIDTH   128
#define OLED_HEIGHT  64

// 页面编号
typedef enum {
    // 温湿度和联网状态
    OLED_PAGE_HOME = 0,
    // 八台设备开关
    OLED_PAGE_DEVICE,
    // 各类读数
    OLED_PAGE_SENSOR,
    // 自检进度
    OLED_PAGE_SELFTEST,
    // WiFi / MQTT / 蓝牙
    OLED_PAGE_NET,
    OLED_PAGE_MAX,
} oled_page_t;

// 把屏幕准备好，没装 U8g2 库就整个跳过
bool oled_ui_init(void);

// 屏幕能用吗
bool oled_ui_is_ready(void);

// 主循环喊这个，到点才刷
void oled_ui_poll(void);

// 翻页
void oled_ui_next_page(void);
void oled_ui_prev_page(void);
void oled_ui_set_page(oled_page_t page);
oled_page_t oled_ui_get_page(void);

// 页面上高亮第几项，0 起
void oled_ui_set_cursor(uint8_t idx);
uint8_t oled_ui_get_cursor(void);

// 这页一共几项，光标最多走到几；没有可选项的页给 0
uint8_t oled_ui_cursor_max(oled_page_t page);

// 页名，日志里看
const char *oled_ui_page_name(oled_page_t page);

// 顶一行字闪一下，比如喊了语音命令
void oled_ui_toast(const char *msg);
