/*
 * 模块：
 *   WS2812 彩灯驱动的对外口子。四个房间的灯带和板上小灯都走这里，
 *   被 led.c 调用，具体实现在 ws2812.c。
 *
 * 功能：
 *   建一条灯带
 *   给灯上色
 *   把颜色发出去
 *   清空灯带
 */
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：灯带的把手 */
typedef struct ws2812_strip_s *ws2812_strip_handle_t;

/* 功能：建一条灯带，最多四条 */
esp_err_t ws2812_new_strip(gpio_num_t gpio, uint32_t led_num, ws2812_strip_handle_t *out);

/* 功能：给一颗上色，先不发 */
esp_err_t ws2812_set_pixel(ws2812_strip_handle_t h, uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/* 功能：整条一个颜色 */
esp_err_t ws2812_set_all(ws2812_strip_handle_t h, uint8_t r, uint8_t g, uint8_t b);

/* 功能：把颜色发出去 */
esp_err_t ws2812_refresh(ws2812_strip_handle_t h);

/* 功能：清成全黑 */
esp_err_t ws2812_clear(ws2812_strip_handle_t h, bool do_refresh);

/* 功能：报灯珠数量 */
uint32_t ws2812_get_num(ws2812_strip_handle_t h);

/* 功能：翻转引脚直接发 */
esp_err_t ws2812_bitbang_write(gpio_num_t gpio, const uint8_t *grb, uint32_t len);

#ifdef __cplusplus
}
#endif
