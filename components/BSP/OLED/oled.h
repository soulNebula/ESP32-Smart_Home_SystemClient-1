/**
 * @file  oled.h
 * @brief SSD1306 128x64 OLED 驱动（I2C，地址 0x3C / 自动回退 0x3D）
 *
 * 【本模块只负责"屏幕"这件事】—— 初始化、探测、清屏、整屏刷新、按页写入。
 *
 * 2026-09-29 清理说明：
 *   这里原先还有一套自带字库（6x8 / 8x16 ASCII + 16x16 中文，由 tools/gen_font.ps1 生成
 *   oled_font.c）和仪表盘 / 开机画面 / 提示框绘制接口。换成 astra UI（u8g2 画布）之后，
 *   oled_draw_str / oled_draw_str_big / oled_printf / oled_draw_hline / oled_draw_rect /
 *   oled_show_dashboard / oled_show_message / oled_show_splash **全工程零调用**，
 *   已连同 oled_font.c/.h 和生成脚本一起删除（省固件体积，少一份要维护的字库）。
 *
 *   现在屏幕内容的渲染链路是：
 *       astra UI 用 u8g2 画进画布 → 按页回调 → oled_write_page()
 *
 * 注意：OLED 不接也能跑 —— oled_is_ready() 返回 false，所有刷新函数变成空操作。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH   128
#define OLED_HEIGHT  64

/** @brief 初始化 OLED（幂等）。没有屏返回 ESP_ERR_NOT_FOUND，但不影响其它功能 */
esp_err_t oled_init(void);

/** @brief 屏幕是否可用 */
bool oled_is_ready(void);

/** @brief 清空内部显存（不上屏）；随后可调 oled_refresh() 推上屏 */
void oled_clear(void);

/** @brief 把内部显存整屏推上屏（8 页，每页 128 字节） */
void oled_refresh(void);

/**
 * @brief 直接写一页（128 字节，竖列格式，bit0=顶行）并上屏
 *
 * 供外部渲染器（astra UI 的 u8g2 画布）按页刷屏用，**不经内部显存**。
 * @param page 页号 0~7
 * @param data 128 字节页数据
 */
void oled_write_page(uint8_t page, const uint8_t *data);

#ifdef __cplusplus
}
#endif
