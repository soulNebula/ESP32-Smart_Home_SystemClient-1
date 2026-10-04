/**
 * @file  led.h
 * @brief 灯带业务层（4 个房间）+ 板载 RGB 状态灯
 *
 * 分层：led.c 只负责「业务语义」（哪个房间、开关、亮度、颜色），
 *       真正产生 WS2812 时序由 ws2812.c 负责。
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 灯带分区（顺序和 board_config.h 里的 GPIO 一一对应） */
typedef enum {
    LED_ZONE_LIVING = 0,   /**< 客厅 GPIO4  */
    LED_ZONE_KITCHEN,      /**< 厨房 GPIO5  */
    LED_ZONE_BEDROOM,      /**< 卧室 GPIO6  */
    LED_ZONE_BATH,         /**< 浴室 GPIO7  */
    LED_ZONE_MAX,
} led_zone_t;

/** 板载状态灯语义 */
typedef enum {
    LED_STATUS_BOOT = 0,        /**< 启动中：蓝色慢闪 */
    LED_STATUS_WIFI_CONNECTING, /**< 连 WiFi：黄色慢闪 */
    LED_STATUS_WIFI_OK,         /**< WiFi 通了：绿色常亮 */
    LED_STATUS_MQTT_OK,         /**< MQTT 也通了：青色常亮 */
    LED_STATUS_ERROR,           /**< 出错：红色快闪 */
    LED_STATUS_AP_MODE,         /**< 配网模式：紫色慢闪 */
} led_status_t;

/** @brief 初始化 4 路灯带 + 板载状态灯（幂等）。单路灯带缺失不影响其它路 */
esp_err_t led_init(void);

/* ---------------- 灯带控制 ---------------- */

esp_err_t led_set_power(led_zone_t zone, bool on);
bool      led_get_power(led_zone_t zone);

/** @brief 设置颜色（0~255）。会自动记住颜色，下次开灯沿用 */
esp_err_t led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b);

/** @brief 亮度百分比 0~100（软件调光，直接缩放 RGB 值） */
esp_err_t led_set_brightness(led_zone_t zone, uint8_t percent);
uint8_t   led_get_brightness(led_zone_t zone);

esp_err_t led_toggle(led_zone_t zone);
esp_err_t led_all_off(void);
esp_err_t led_all_on(void);

/** @brief 把当前状态重新刷到硬件（改完一批再统一刷，避免闪烁） */
esp_err_t led_flush(void);

/** @brief 分区英文名，用于日志 / MQTT / OLED，如 "living" */
const char *led_zone_name(led_zone_t zone);

/** @brief 由名字反查分区，找不到返回 LED_ZONE_MAX */
led_zone_t led_zone_from_name(const char *name);

/* ---------------- 板载状态灯 ---------------- */

esp_err_t led_status_set(led_status_t st);
esp_err_t led_status_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
