#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 四个房间的灯
typedef enum {
    // 客厅那路
    LED_ZONE_LIVING = 0,
    // 厨房那路
    LED_ZONE_KITCHEN,
    // 卧室那路
    LED_ZONE_BEDROOM,
    // 浴室那路
    LED_ZONE_BATH,
    LED_ZONE_MAX,
} led_zone_t;

// 状态灯的花样
typedef enum {
    // 启动蓝灯慢闪
    LED_STATUS_BOOT = 0,
    // 连网黄灯慢闪
    LED_STATUS_WIFI_CONNECTING,
    // 网通绿灯常亮
    LED_STATUS_WIFI_OK,
    // 上云青灯常亮
    LED_STATUS_MQTT_OK,
    // 出错红灯快闪
    LED_STATUS_ERROR,
    // 配网紫灯慢闪
    LED_STATUS_AP_MODE,
} led_status_t;

// 把硬件准备好
esp_err_t led_init(void);

esp_err_t led_set_power(led_zone_t zone, bool on);
bool      led_get_power(led_zone_t zone);

// 设颜色并记住它
esp_err_t led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b);

// 设亮度百分比
esp_err_t led_set_brightness(led_zone_t zone, uint8_t percent);
uint8_t   led_get_brightness(led_zone_t zone);

esp_err_t led_toggle(led_zone_t zone);
esp_err_t led_all_off(void);
esp_err_t led_all_on(void);

// 改完一批再统一刷
esp_err_t led_flush(void);

// 分区号换英文名
const char *led_zone_name(led_zone_t zone);

// 板载状态灯
esp_err_t led_status_set(led_status_t st);
esp_err_t led_status_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
