#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

// 起服务，可重复调
esp_err_t mqtt_app_start(void);

// 看连上没有
bool mqtt_is_connected(void);

// 马上报一遍状态
esp_err_t mqtt_publish_state(void);

// 马上报一遍测量
esp_err_t mqtt_publish_sensor(const sensor_data_t *d);

// 报一遍联动设置
esp_err_t mqtt_publish_config(void);

// 回一条执行结果
esp_err_t mqtt_publish_ack(const char *what, bool ok, const char *detail);

// 报按键语音事件
esp_err_t mqtt_publish_event(const char *what);

// 设备一变就自动报
esp_err_t mqtt_app_bind_device_events(void);

#ifdef __cplusplus
}
#endif
