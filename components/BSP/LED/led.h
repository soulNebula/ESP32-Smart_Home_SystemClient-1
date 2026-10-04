/*
 * 模块：
 *   灯带的对外接口，管四个房间的灯，外加板上一颗状态灯。
 *   被 device_model.c 和 main.c 调用，向下由 ws2812.c 出灯珠时序。
 *
 * 功能：
 *   开关某路灯
 *   调亮度和颜色
 *   整批一起刷新
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：四个房间的灯 */
typedef enum {
    LED_ZONE_LIVING = 0,   /* 功能：客厅那路 */
    LED_ZONE_KITCHEN,      /* 功能：厨房那路 */
    LED_ZONE_BEDROOM,      /* 功能：卧室那路 */
    LED_ZONE_BATH,         /* 功能：浴室那路 */
    LED_ZONE_MAX,
} led_zone_t;

/* 功能：状态灯的花样 */
typedef enum {
    LED_STATUS_BOOT = 0,        /* 功能：启动蓝灯慢闪 */
    LED_STATUS_WIFI_CONNECTING, /* 功能：连网黄灯慢闪 */
    LED_STATUS_WIFI_OK,         /* 功能：网通绿灯常亮 */
    LED_STATUS_MQTT_OK,         /* 功能：上云青灯常亮 */
    LED_STATUS_ERROR,           /* 功能：出错红灯快闪 */
    LED_STATUS_AP_MODE,         /* 功能：配网紫灯慢闪 */
} led_status_t;

/* 功能：把硬件准备好 */
esp_err_t led_init(void);

esp_err_t led_set_power(led_zone_t zone, bool on);
bool      led_get_power(led_zone_t zone);

/* 功能：设颜色并记住它 */
esp_err_t led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b);

/* 功能：设亮度百分比 */
esp_err_t led_set_brightness(led_zone_t zone, uint8_t percent);
uint8_t   led_get_brightness(led_zone_t zone);

esp_err_t led_toggle(led_zone_t zone);
esp_err_t led_all_off(void);
esp_err_t led_all_on(void);

/* 功能：改完一批再统一刷 */
esp_err_t led_flush(void);

/* 功能：分区号换英文名 */
const char *led_zone_name(led_zone_t zone);

/* 功能：英文名反查分区号 */
led_zone_t led_zone_from_name(const char *name);

/* 功能：板载状态灯 */
esp_err_t led_status_set(led_status_t st);
esp_err_t led_status_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
