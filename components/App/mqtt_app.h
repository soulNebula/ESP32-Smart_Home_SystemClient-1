/*
 * 模块：
 *   上网这条链路的口子。给 main.c 调用，具体活儿都在 mqtt_app.c 里干。
 *   手机走上网和走蓝牙下发的是同一套 JSON，解析命令只有一份：
 *   声明在 app_cmd.h，实现在 mqtt_app.c，蓝牙那边也调它。
 *
 * 功能：
 *   起服务
 *   看连上没有
 *   报状态
 *   报测量
 *   报阈值
 *   回执和事件
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：起服务，可重复调 */
esp_err_t mqtt_app_start(void);

/* 功能：看连上没有 */
bool mqtt_is_connected(void);

/* 功能：马上报一遍状态 */
esp_err_t mqtt_publish_state(void);

/* 功能：马上报一遍测量 */
esp_err_t mqtt_publish_sensor(const sensor_data_t *d);

/* 功能：报一遍联动设置 */
esp_err_t mqtt_publish_config(void);

/* 功能：回一条执行结果 */
esp_err_t mqtt_publish_ack(const char *what, bool ok, const char *detail);

/* 功能：报按键语音事件 */
esp_err_t mqtt_publish_event(const char *what);

/* 功能：设备一变就自动报 */
esp_err_t mqtt_app_bind_device_events(void);

#ifdef __cplusplus
}
#endif
