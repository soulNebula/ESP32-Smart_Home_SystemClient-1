/*
 * 模块：
 *   语音的对外接口。说一句话算哪条命令，都在这里定好；
 *   被 main.c 和界面层 include，由 voice.c 实现。
 *   真正接模块或麦克风的活在同目录的 voice.c、voice_esp_sr.c 里做。
 *
 * 能做这些：
 *   定好命令清单
 *   开机准备好
 *   登记回话口
 *   发文字让模块报
 *   报温度湿度
 *   自己造一条命令
 *   名字和命令号互查
 *   看模块在不在线
 *
 * 几句说明：
 *   这里的命令清单就是全部能听懂的词，从"没听清"到自动联动总开关，
 *   一共二十六项；voice.c 里那张名字对照表的顺序必须和它一模一样。
 *   报温度湿度的时候，哪一项传负数就不报那一项，这样一句话可以只报
 *   其中一个值；两项都是负数就当参数不对，直接返回不播报。
 *   自己造命令是给硬件没到的时候用的，按键、串口、手机都能调，
 *   走的是和真模块完全一样的路，方便先把整条业务链跑通。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：说一句话算哪条命令 */
typedef enum {
    VOICE_CMD_NONE = 0,

    /* 功能：单个房间的灯 */
    VOICE_CMD_LED_LIVING_ON,
    VOICE_CMD_LED_LIVING_OFF,
    VOICE_CMD_LED_KITCHEN_ON,
    VOICE_CMD_LED_KITCHEN_OFF,
    VOICE_CMD_LED_BEDROOM_ON,
    VOICE_CMD_LED_BEDROOM_OFF,
    VOICE_CMD_LED_BATH_ON,
    VOICE_CMD_LED_BATH_OFF,

    /* 功能：所有灯一起 */
    VOICE_CMD_LED_ALL_ON,
    VOICE_CMD_LED_ALL_OFF,

    /* 功能：风扇 */
    VOICE_CMD_FAN_ON,
    VOICE_CMD_FAN_OFF,

    /* 功能：窗户 */
    VOICE_CMD_WINDOW_OPEN,
    VOICE_CMD_WINDOW_CLOSE,

    /* 功能：门 */
    VOICE_CMD_DOOR_OPEN,
    VOICE_CMD_DOOR_CLOSE,

    /* 功能：窗帘 */
    VOICE_CMD_CURTAIN_OPEN,
    VOICE_CMD_CURTAIN_CLOSE,

    /* 功能：问一句设备状态 */
    VOICE_CMD_QUERY_TEMP,
    VOICE_CMD_QUERY_HUMI,
    VOICE_CMD_QUERY_LIGHT,
    VOICE_CMD_QUERY_ALL,
    VOICE_CMD_QUERY_STATUS,

    /* 功能：自动联动的总开关 */
    VOICE_CMD_AUTO_ON,
    VOICE_CMD_AUTO_OFF,

    VOICE_CMD_MAX,
} voice_cmd_t;

/* 功能：认出来之后叫谁 */
typedef void (*voice_cmd_cb_t)(voice_cmd_t cmd, void *user_data);

/* 功能：开机把语音准备好 */
esp_err_t voice_init(void);

/* 功能：登记上层的回话口 */
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data);

/* 功能：发一段字让模块念 */
esp_err_t voice_speak(const char *text);

/* 功能：报温度湿度 */
esp_err_t voice_speak_temp(float temp, float humi);

/* 功能：自己造一条命令 */
esp_err_t voice_inject_cmd(voice_cmd_t cmd);

/* 功能：命令号翻名字 */
const char *voice_cmd_name(voice_cmd_t cmd);

/* 功能：名字翻回命令号 */
voice_cmd_t voice_cmd_from_name(const char *name);

/* 功能：看模块还接着没 */
bool voice_is_online(void);

#ifdef __cplusplus
}
#endif
