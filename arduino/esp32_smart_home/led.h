#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// ======================================================================
// 四路房间灯 + 板载状态灯
//
// 对应 ESP-IDF 工程的 components/BSP/LED/led.c 的"普通单色 LED"后端。
// 这版 LED 模块是 3 脚白光模块（VCC/GND/S），用 LEDC 调亮度，颜色只记状态。
// ======================================================================

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
} led_status_t;

// 把硬件准备好
bool led_init(void);

bool led_set_power(led_zone_t zone, bool on);
bool led_get_power(led_zone_t zone);

// 设颜色并记住它，硬件是白光，只是记着
bool led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b);
bool led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b);

// 设亮度百分比
bool led_set_brightness(led_zone_t zone, uint8_t percent);
uint8_t led_get_brightness(led_zone_t zone);

bool led_toggle(led_zone_t zone);
bool led_all_off(void);
bool led_all_on(void);

// 改完一批再统一刷
bool led_flush(void);

// 分区号换英文名
const char *led_zone_name(led_zone_t zone);

// 板载状态灯，改完要主循环喊 led_poll() 才有闪烁
bool led_status_set(led_status_t st);

// 主循环喊这个，状态灯的花样靠它走
void led_poll(void);
