/**
 * @file  ws2812.h
 * @brief WS2812/WS2812B 灯带驱动
 *
 * 两套后端：
 *   1) RMT 后端（ws2812_new_strip / set_pixel / refresh）—— 用于 4 个房间的灯带。
 *      ESP32-S3 只有 4 个 RMT TX 通道，正好被 4 路灯带占满。
 *   2) 位带后端（ws2812_bitbang_write）—— 用于板载状态灯。
 *      刷新时关中断约 30us，状态灯刷新频率极低，无影响。
 *
 * 不依赖 espressif/led_strip 在线组件，保证离线可编译。
 */
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 灯带句柄（不透明） */
typedef struct ws2812_strip_s *ws2812_strip_handle_t;

/**
 * @brief 创建一条 RMT 驱动的灯带
 * @param gpio     数据脚
 * @param led_num  灯珠数量
 * @param out      返回句柄
 * @note 最多支持 4 条（受 S3 的 RMT TX 通道数限制），超了返回 ESP_ERR_NO_MEM
 */
esp_err_t ws2812_new_strip(gpio_num_t gpio, uint32_t led_num, ws2812_strip_handle_t *out);

/** @brief 设置单个像素（写内存，不立即发送；需调用 ws2812_refresh） */
esp_err_t ws2812_set_pixel(ws2812_strip_handle_t h, uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/** @brief 整条灯带设为同一颜色（写内存） */
esp_err_t ws2812_set_all(ws2812_strip_handle_t h, uint8_t r, uint8_t g, uint8_t b);

/** @brief 把内存里的像素数据真正发到灯带（阻塞，约 led_num*30us） */
esp_err_t ws2812_refresh(ws2812_strip_handle_t h);

/** @brief 清空为全黑 */
esp_err_t ws2812_clear(ws2812_strip_handle_t h, bool do_refresh);

/** @brief 灯珠数量 */
uint32_t ws2812_get_num(ws2812_strip_handle_t h);

/**
 * @brief 位带方式一次性写出若干颗 WS2812（GRB 顺序）
 * @param gpio  数据脚
 * @param grb   数据缓冲，长度 = len*3，字节顺序 G,R,B
 * @param len   灯珠数量
 */
esp_err_t ws2812_bitbang_write(gpio_num_t gpio, const uint8_t *grb, uint32_t len);

#ifdef __cplusplus
}
#endif
