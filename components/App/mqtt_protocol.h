#pragma once

#include <stddef.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 字段名两边一起改
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

// 拼主题的几个口子

// 拼出基础主题
esp_err_t mqtt_topic_base(char *buf, size_t len);

// 拼出状态主题
esp_err_t mqtt_topic_state(char *buf, size_t len);

// 拼出测量主题
esp_err_t mqtt_topic_sensor(char *buf, size_t len);

// 拼出在线主题
esp_err_t mqtt_topic_availability(char *buf, size_t len);

// 拼出回执主题
esp_err_t mqtt_topic_ack(char *buf, size_t len);

// 拼出事件主题
esp_err_t mqtt_topic_event(char *buf, size_t len);

// 拼出命令主题
esp_err_t mqtt_topic_cmd(char *buf, size_t len);

// 拼出命令通配主题
esp_err_t mqtt_topic_cmd_wildcard(char *buf, size_t len);

// 拼出阈值主题
esp_err_t mqtt_topic_config(char *buf, size_t len);

// 拼出查询主题
esp_err_t mqtt_topic_get(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
