/*
 * 模块：
 *   自动联动。人不动手的时候它自己看着办：隔一会儿收一次传感器的数，
 *   觉得该动就去改设备总状态表（device_model.c）—— 那是全屋唯一的
 *   真相来源，改完由它统一动硬件、通知界面和手机。
 *   被 main.c 的主循环定期喊一次；自己读 sensor.c 的光照、温度、雨滴。
 *   几条线（阈值）能由手机经 MQTT 改，改完存在 flash 里，不用重烧固件。
 *
 *   三条规则：
 *     一、看光照。屋里暗过下面那条线，就开客厅灯，同时把窗帘拉上；
 *         屋里亮过上面那条线，就关客厅灯，同时把窗帘拉开。
 *         灯和窗帘的动作正好相反 —— 暗了人想开灯，可窗帘得拉上挡外面；
 *         亮了人想关灯，窗帘就拉开透光。
 *     二、看温度。热过上面那条线就开风扇（按设定转速），凉过下面那条线才关。
 *     三、看雨滴。湿度超过那条线就当在下雨，先把窗户关上；雨停之后要不要
 *         自动开窗，看 auto_window_reopen，默认不开，怕雨又来了。
 *
 *   两处讲究：
 *     一是开和关各用一条线，两条线中间那段谁也别动 —— 数值在线上来回抖时
 *     不会一会儿开一会儿关，灯不闪、风扇不响、窗帘舵机也不来回转。
 *     二是人优先。哪个设备刚被人用手机、语音或按键动过，一分钟之内
 *     这三条规则都不许碰它，免得刚开的灯又被自己的规则关掉。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_config.h"
#include "sensor.h"
#include "device_model.h"   /* 功能：要用设备编号 */

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：联动用的那几条线 */
typedef struct {
    bool  enabled;             /* 功能：自动总开关 */
    float light_on_lux;        /* 功能：暗过这条就开灯 */
    float light_off_lux;       /* 功能：亮过这条就关灯 */
    float temp_fan_on_c;       /* 功能：热过这条开风扇 */
    float temp_fan_off_c;      /* 功能：凉过这条关风扇 */
    uint8_t fan_auto_speed;    /* 功能：自动开风扇的转速 */
    float rain_pct;            /* 功能：湿过这条当在下雨 */
    bool  auto_window_reopen;  /* 功能：雨停要不要自动开窗 */
    bool  auto_light_enable;   /* 功能：光照这条要不要用 */
    bool  auto_temp_enable;    /* 功能：温度这条要不要用 */
    bool  auto_rain_enable;    /* 功能：雨滴这条要不要用 */
} automation_cfg_t;

/* 功能：上电读回存的几条线 */
esp_err_t automation_init(void);

esp_err_t automation_set_enabled(bool enabled);
bool      automation_is_enabled(void);

/* 功能：拿配置去改，改完存 */
automation_cfg_t *automation_get_cfg(void);

/* 功能：存起来，掉电不丢 */
esp_err_t automation_save(void);

/* 功能：从存的地方读回来 */
esp_err_t automation_load(void);

/* 功能：按名字改一条线 */
esp_err_t automation_set_threshold(const char *key, float value);

/* 功能：几条线拼成 JSON */
int automation_cfg_json(char *buf, size_t len);

/* 功能：主循环隔会儿喊一次 */
void automation_tick(const sensor_data_t *d);

/* 功能：告诉联动刚有人动手 */
void automation_notify_manual(device_id_t id);

/* 功能：手动之后歇多久不碰 */
#define AUTO_MANUAL_GUARD_MS  60000

#ifdef __cplusplus
}
#endif
