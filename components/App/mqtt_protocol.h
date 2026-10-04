/**
 * @file  mqtt_protocol.h
 * @brief MQTT 通信协议定义（设备 ↔ 手机 App / 上位机）
 *
 * ============================ Topic 规划 ============================
 *  基础前缀 base = <CONFIG_APP_MQTT_TOPIC_PREFIX>/<唯一ID>
 *  唯一ID 默认取 MAC 后 3 字节，例如 "a1b2c3" ——
 *  这是为了在公共测试 broker 上避免和别人撞车，务必保持唯一。
 *
 *  方向  Topic                                  说明
 *  ----  -------------------------------------  --------------------------------
 *  上行  <base>/availability                     "online"/"offline"，retain，LWT
 *  上行  <base>/state                            全设备状态 JSON，retain
 *  上行  <base>/sensor                           传感器数据 JSON（周期见
 *                                                CONFIG_APP_MQTT_PUBLISH_SENSOR_MS，默认 2s）
 *  上行  <base>/ack                              命令执行结果
 *  上行  <base>/event                            按键/语音触发的动作事件
 *  下行  <base>/cmd                              控制命令 JSON
 *  下行  <base>/config                           改阈值/开关自动模式
 *  下行  <base>/get                              payload 任意 → 立刻回一次 state+sensor
 *
 * ============================ 下行命令 JSON ============================
 *  {"dev":"led_living","action":"on"}
 *  {"dev":"led_living","action":"off"}
 *  {"dev":"led_living","action":"toggle"}
 *  {"dev":"led_living","action":"set","value":60}          // 亮度 60%
 *  {"dev":"led_living","action":"color","r":255,"g":0,"b":0}
 *  {"dev":"fan","action":"set","value":75}                 // 转速 75%
 *  {"dev":"window","action":"open"}                        // 等价 set value=100
 *  {"dev":"window","action":"close"}                       // 等价 set value=0
 *  {"dev":"all","action":"off"}                            // 全部关闭
 *  {"action":"auto","value":true}                          // 自动模式开关
 *
 *  dev 取值：led_living / led_kitchen / led_bedroom / led_bath /
 *            fan / window / door / curtain / all
 *
 * ============================ 下行配置 JSON ============================
 *  {"light_on_lux":80,"temp_fan_on_c":28,"rain_pct":30,
 *   "enabled":true,"fan_auto_speed":70,"auto_window_reopen":false}
 *
 * ============================ 上行 state JSON ============================
 *  {"led_living":{"power":true,"level":80,"r":255,"g":255,"b":255},
 *   "fan":{"power":false,"level":0},
 *   "window":{"power":false,"level":0},
 *   "door":{"power":false,"level":0},
 *   "curtain":{"power":false,"level":0},
 *   "auto":true,"rssi":-52,"ip":"192.168.1.23","uptime":1234}
 *
 * ============================ 上行 sensor JSON ============================
 *  {"temp":26.5,"humi":58.2,"temp_valid":true,
 *   "lux":123.4,"light_pct":48.0,"light_is_bh1750":true,
 *   "rain":12.5,"rain_detected":false}
 *
 * ============================ 手机端怎么测 ============================
 *  用 MQTTX（手机/PC 都有）连 broker.emqx.io:1883。
 *
 *  假设你的设备 ID 是 a1b2c3（= MAC 后 3 字节，见 CONFIG_APP_MQTT_UID_OVERRIDE）：
 *    ★ 订阅（看状态）：smarthome/a1b2c3/state
 *                      想一次看全部设备可用通配：smarthome/+/state
 *    ★ 发布（下命令）：smarthome/a1b2c3/cmd      payload 用下面「下行命令 JSON」
 *
 *  ⚠ base 只有 2 段（前缀 / ID），所以 <base>/state 一共 3 段。
 *    写成 smarthome/+/a1b2c3/state（4 段）是匹配不到的 —— 这是常见笔误。
 */
#pragma once

#include <stddef.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- JSON 字段名（改这里要同步改文档和手机端） ---------------- */
#define MQTT_KEY_DEV        "dev"
#define MQTT_KEY_ACTION     "action"
#define MQTT_KEY_VALUE      "value"
#define MQTT_KEY_R          "r"
#define MQTT_KEY_G          "g"
#define MQTT_KEY_B          "b"

#define MQTT_ACTION_ON      "on"
#define MQTT_ACTION_OFF     "off"
#define MQTT_ACTION_TOGGLE  "toggle"
#define MQTT_ACTION_SET     "set"
#define MQTT_ACTION_COLOR   "color"
#define MQTT_ACTION_OPEN    "open"
#define MQTT_ACTION_CLOSE   "close"
#define MQTT_ACTION_AUTO    "auto"

#define MQTT_PAYLOAD_ONLINE   "online"
#define MQTT_PAYLOAD_OFFLINE  "offline"

/* ---------------- Topic 拼装（都用 base = prefix/uid） ---------------- */

/** @brief 取基础前缀，如 "smarthome/a1b2c3" */
esp_err_t mqtt_topic_base(char *buf, size_t len);

/** @brief 上行：设备状态（retain） */
esp_err_t mqtt_topic_state(char *buf, size_t len);

/** @brief 上行：传感器数据 */
esp_err_t mqtt_topic_sensor(char *buf, size_t len);

/** @brief 上行：在线状态（retain + LWT） */
esp_err_t mqtt_topic_availability(char *buf, size_t len);

/** @brief 上行：命令应答 */
esp_err_t mqtt_topic_ack(char *buf, size_t len);

/** @brief 上行：本地事件（按键/语音） */
esp_err_t mqtt_topic_event(char *buf, size_t len);

/** @brief 下行：控制命令（用来订阅，精确） */
esp_err_t mqtt_topic_cmd(char *buf, size_t len);

/** @brief 下行：控制命令通配订阅 "<base>/cmd/#" */
esp_err_t mqtt_topic_cmd_wildcard(char *buf, size_t len);

/** @brief 下行：阈值配置 */
esp_err_t mqtt_topic_config(char *buf, size_t len);

/** @brief 下行：主动查询（payload 任意 → 回 state + sensor） */
esp_err_t mqtt_topic_get(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
