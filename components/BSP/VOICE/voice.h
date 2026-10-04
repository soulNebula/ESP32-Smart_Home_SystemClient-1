/**
 * @file  voice.h
 * @brief 语音交互【抽象层】—— 硬件没到也能把整条业务链跑通
 *
 * 设计意图：
 *   board_config.h 里已经预留好 UART1（ESP32 TX=GPIO47 → 模块 RXD，ESP32 RX=GPIO21
 *   ← 模块 TXD，9600 8N1）。语音模块（SU-03T / ASRPRO / LD3320 等）到货后，
 *   只要它输出的是"命令词序号"或约定的短字符串，就能直接对接。
 *
 *   但如果现在就等硬件，整个项目没法推进。所以本模块把「说一句话」
 *   抽象成 voice_cmd_t 枚举 + 一个回调：
 *     · 真实模块  → voice_uart.c 解析串口字节，翻译成 voice_cmd_t
 *     · 手动注入  → voice_inject_cmd()（按键 / 串口调试 / MQTT 都能调）
 *   两种来源走【完全相同】的下游分支（App 里的 voice_cmd_cb）。
 *
 *   将来换成"离线 ESP-SR（唤醒词+命令词）"或"在线云识别"，
 *   也只需要替换 voice.c 内部实现，App 层一行都不用改。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 语音指令集 —— 覆盖需求 2/4/6：所有灯、风扇、窗、门、窗帘、以及查询温湿度 */
typedef enum {
    VOICE_CMD_NONE = 0,

    /* 单个房间灯 */
    VOICE_CMD_LED_LIVING_ON,
    VOICE_CMD_LED_LIVING_OFF,
    VOICE_CMD_LED_KITCHEN_ON,
    VOICE_CMD_LED_KITCHEN_OFF,
    VOICE_CMD_LED_BEDROOM_ON,
    VOICE_CMD_LED_BEDROOM_OFF,
    VOICE_CMD_LED_BATH_ON,
    VOICE_CMD_LED_BATH_OFF,

    /* 全部灯 */
    VOICE_CMD_LED_ALL_ON,
    VOICE_CMD_LED_ALL_OFF,

    /* 风扇 */
    VOICE_CMD_FAN_ON,
    VOICE_CMD_FAN_OFF,

    /* 窗户 */
    VOICE_CMD_WINDOW_OPEN,
    VOICE_CMD_WINDOW_CLOSE,

    /* 门 */
    VOICE_CMD_DOOR_OPEN,
    VOICE_CMD_DOOR_CLOSE,

    /* 窗帘 */
    VOICE_CMD_CURTAIN_OPEN,
    VOICE_CMD_CURTAIN_CLOSE,

    /* 查询播报 */
    VOICE_CMD_QUERY_TEMP,
    VOICE_CMD_QUERY_HUMI,
    VOICE_CMD_QUERY_LIGHT,
    VOICE_CMD_QUERY_ALL,
    VOICE_CMD_QUERY_STATUS,

    /* 自动化总开关 */
    VOICE_CMD_AUTO_ON,
    VOICE_CMD_AUTO_OFF,

    VOICE_CMD_MAX,
} voice_cmd_t;

typedef void (*voice_cmd_cb_t)(voice_cmd_t cmd, void *user_data);

/** @brief 初始化语音模块串口（幂等）。模块没接也不报错，只是收不到指令 */
esp_err_t voice_init(void);

/** @brief 注册指令回调（只保留最后一个） */
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data);

/**
 * @brief 请求语音模块 TTS 播报一段文本
 * @note  把文本经 UART 发给模块，末尾补 CRLF。
 *        模块若不支持文本 TTS，它会自行忽略这段数据，对系统无副作用，
 *        因此本函数仍然返回 ESP_OK。
 *        只有两种情况会失败：UART 未初始化 → ESP_ERR_INVALID_STATE；
 *        text 为 NULL → ESP_ERR_INVALID_ARG。
 */
esp_err_t voice_speak(const char *text);

/**
 * @brief 播报温湿度
 *
 * ★ 约定：传【负数】表示"这一项不播报"，这样一句话可以只播其中一个值。
 *     voice_speak_temp(26.5,  58.0) → "当前温度26.5度，湿度58%"
 *     voice_speak_temp(26.5,  -1.0) → 只播温度
 *     voice_speak_temp(-100,  58.0) → 只播湿度
 *     两项都是负数                  → 返回 ESP_ERR_INVALID_ARG，不播报
 */
esp_err_t voice_speak_temp(float temp, float humi);

/**
 * @brief 手动注入一条指令 —— 驱动下游业务逻辑
 *
 * 用途：① 配件没到，用按键/串口/MQTT 模拟语音指令做全链路验证
 *       ② 单元测试
 */
esp_err_t voice_inject_cmd(voice_cmd_t cmd);

/** @brief 指令名（"led_living_on" ...），用于日志 / MQTT */
const char *voice_cmd_name(voice_cmd_t cmd);

/** @brief 由名字反查指令，找不到返回 VOICE_CMD_NONE */
voice_cmd_t voice_cmd_from_name(const char *name);

/** @brief 语音模块是否在线（能收到字节即视为在线） */
bool voice_is_online(void);

#ifdef __cplusplus
}
#endif
