#pragma once

#include <stddef.h>

#include "device_model.h"

// ======================================================================
// 命令分发：网络和蓝牙收到的是同一套 JSON，所以只用一份解析
//
// 对应 ESP-IDF 工程的 components/App/app_cmd.h
// 加 mqtt_app.c 里那段 app_cmd_handle_json。
//
// 收的报文长这样：
//   {"dev":"led_living","action":"on"}
//   {"dev":"fan","action":"set","value":60}
//   {"dev":"led_living","action":"color","r":255,"g":0,"b":0}
//   {"action":"auto","value":true}
// ======================================================================

// 字段名两边一起改
#define CMD_KEY_DEV        "dev"
#define CMD_KEY_ACTION     "action"
#define CMD_KEY_VALUE      "value"
#define CMD_KEY_R          "r"
#define CMD_KEY_G          "g"
#define CMD_KEY_B          "b"

#define CMD_ACTION_ON      "on"
#define CMD_ACTION_OFF     "off"
#define CMD_ACTION_TOGGLE  "toggle"
#define CMD_ACTION_SET     "set"
#define CMD_ACTION_COLOR   "color"
#define CMD_ACTION_OPEN    "open"
#define CMD_ACTION_CLOSE   "close"
#define CMD_ACTION_AUTO    "auto"

// 解一条命令，来源写 SRC_MQTT 或 SRC_BLE
bool app_cmd_handle_json(const char *json, int len, ctrl_source_t src);

// 从 JSON 里抠一个字符串字段，抠到给 true
bool json_get_str(const char *json, const char *key, char *out, size_t len);

// 从 JSON 里抠一个数字字段，抠到给 true
bool json_get_num(const char *json, const char *key, double *out);
