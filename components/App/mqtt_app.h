/**
 * @file  mqtt_app.h
 * @brief MQTT 客户端封装（esp-mqtt）—— 对应需求 7：手机 App / 上位机控制
 *
 * 特性：
 *   · 断线自动重连（esp-mqtt 内建）
 *   · LWT 遗嘱消息：设备掉线时 broker 自动发 offline
 *   · 上电后主动上报一次 state + sensor
 *   · 设备状态变化 / 传感器更新都会自动 publish
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动 MQTT 客户端并订阅下行 topic（幂等，非阻塞） */
esp_err_t mqtt_app_start(void);

/** @brief 是否已连上 broker */
bool mqtt_is_connected(void);

/** @brief 立即上报一次全设备状态 */
esp_err_t mqtt_publish_state(void);

/** @brief 立即上报一次传感器数据 */
esp_err_t mqtt_publish_sensor(const sensor_data_t *d);

/** @brief 上报自动化配置 */
esp_err_t mqtt_publish_config(void);

/** @brief 上报命令应答："ok"/"err" 附带说明 */
esp_err_t mqtt_publish_ack(const char *what, bool ok, const char *detail);

/** @brief 上报本地事件（按键/语音触发的动作） */
esp_err_t mqtt_publish_event(const char *what);

/**
 * @brief 注册"状态变化 → 自动上报"的钩子
 * @note 由 device_model_init 之后调用；内部会顺带把 automation 的手动保护期接上
 */
esp_err_t mqtt_app_bind_device_events(void);

#ifdef __cplusplus
}
#endif
