/*
 * 模块：
 *   定协议：管收发那几条主题名，还有字段和动作都叫什么。
 *   这份约定手机端也照抄，改一个字两边都得改。
 *   手机走上网和走蓝牙下发的是同一套 JSON，认命令只有一份：
 *   声明在 app_cmd.h，实现在 mqtt_app.c，两条链路都调它。
 *
 * 功能：
 *   约定命令怎么写
 *   约定阈值怎么写
 *   约定状态怎么写
 *   约定测量怎么写
 *   命令带设备名动作
 *   亮度风速用数值
 *   颜色用红绿蓝三值
 *   列清各条主题
 */
#pragma once

#include <stddef.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：字段名两边一起改 */
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

/* 功能：拼主题的几个口子 */

/* 功能：拼出基础主题 */
esp_err_t mqtt_topic_base(char *buf, size_t len);

/* 功能：拼出状态主题 */
esp_err_t mqtt_topic_state(char *buf, size_t len);

/* 功能：拼出测量主题 */
esp_err_t mqtt_topic_sensor(char *buf, size_t len);

/* 功能：拼出在线主题 */
esp_err_t mqtt_topic_availability(char *buf, size_t len);

/* 功能：拼出回执主题 */
esp_err_t mqtt_topic_ack(char *buf, size_t len);

/* 功能：拼出事件主题 */
esp_err_t mqtt_topic_event(char *buf, size_t len);

/* 功能：拼出命令主题 */
esp_err_t mqtt_topic_cmd(char *buf, size_t len);

/* 功能：拼出命令通配主题 */
esp_err_t mqtt_topic_cmd_wildcard(char *buf, size_t len);

/* 功能：拼出阈值主题 */
esp_err_t mqtt_topic_config(char *buf, size_t len);

/* 功能：拼出查询主题 */
esp_err_t mqtt_topic_get(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
