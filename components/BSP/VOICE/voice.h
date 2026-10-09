#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 说一句话算哪条命令
typedef enum {
    VOICE_CMD_NONE = 0,

    // 单个房间的灯
    VOICE_CMD_LED_LIVING_ON,
    VOICE_CMD_LED_LIVING_OFF,
    VOICE_CMD_LED_KITCHEN_ON,
    VOICE_CMD_LED_KITCHEN_OFF,
    VOICE_CMD_LED_BEDROOM_ON,
    VOICE_CMD_LED_BEDROOM_OFF,
    VOICE_CMD_LED_BATH_ON,
    VOICE_CMD_LED_BATH_OFF,

    // 所有灯一起
    VOICE_CMD_LED_ALL_ON,
    VOICE_CMD_LED_ALL_OFF,

    // 风扇
    VOICE_CMD_FAN_ON,
    VOICE_CMD_FAN_OFF,

    // 窗户
    VOICE_CMD_WINDOW_OPEN,
    VOICE_CMD_WINDOW_CLOSE,

    // 门
    VOICE_CMD_DOOR_OPEN,
    VOICE_CMD_DOOR_CLOSE,

    // 窗帘
    VOICE_CMD_CURTAIN_OPEN,
    VOICE_CMD_CURTAIN_CLOSE,

    // 问一句设备状态
    VOICE_CMD_QUERY_TEMP,
    VOICE_CMD_QUERY_HUMI,
    VOICE_CMD_QUERY_LIGHT,
    VOICE_CMD_QUERY_ALL,
    VOICE_CMD_QUERY_STATUS,

    // 自动联动的总开关
    VOICE_CMD_AUTO_ON,
    VOICE_CMD_AUTO_OFF,

    VOICE_CMD_MAX,
} voice_cmd_t;

// 认出来之后叫谁
typedef void (*voice_cmd_cb_t)(voice_cmd_t cmd, void *user_data);

// 开机把语音准备好
esp_err_t voice_init(void);

// 登记上层的回话口
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data);

// 自己造一条命令
esp_err_t voice_inject_cmd(voice_cmd_t cmd);

// 命令号翻名字
const char *voice_cmd_name(voice_cmd_t cmd);

// 名字翻回命令号
voice_cmd_t voice_cmd_from_name(const char *name);

#ifdef __cplusplus
}
#endif
