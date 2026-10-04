/**
 * @file  adkey.h
 * @brief 五位 AD 键盘驱动（外接模块，信号线接 IO10 = ADC1_CH9）
 *
 * 【硬件背景】五位 AD 键盘模块（五档电阻分压，空闲 = VCC）：
 *   - 空闲输出 = VCC(3.3V)，ADC 饱和 ≈3128mV → 高于（基线-3mV）视为无按键
 *   - 分压实测（2026-09-28 本机）：
 *         方向键 4 档 ≈ 615 / 1380 / 1968 / 2638 mV
 *         ★ OK 键   = 低压档 ≈ 0mV（模块把 OK 直接对地）
 *     所以 OK 不能用"贴近空闲"的窄带判，必须显式命中低压档
 *     （adkey.c 的 ADKEY_OK_LOW_MAX_MV；历史上漏了这一档，导致按 OK 无反应）
 *   - 方向键阈值匹配容差 ±200mV，覆盖分压电阻 ±10% 偏差
 *   - 换模块后：按一遍各键，看串口里 `ADKEY: 标定: 键X 最低 NNNmV` 那几行，
 *     把新值填进 adkey.c 的 s_key_mv[] / 调整 OK 档位即可
 *
 * 键位映射（ADKEY_1~4 与键盘丝印的对应关系见 adkey.c 顶部说明，
 * 键面是 1/2/3/4/OK 五个印字；如果实机识别错位，改 adkey.c 里的映射表即可）。
 *
 * 事件语义与 key.h 对齐：DOWN / UP / CLICK / LONG_PRESS（长按 2 秒）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADKEY_1 = 0,      /**< 键盘丝印 1 */
    ADKEY_2,          /**< 键盘丝印 2 */
    ADKEY_3,          /**< 键盘丝印 3 */
    ADKEY_4,          /**< 键盘丝印 4 */
    ADKEY_OK,         /**< 键盘丝印 OK */
    ADKEY_NUM,
} adkey_id_t;

typedef enum {
    ADKEY_EVENT_DOWN = 0,      /**< 按下（已消抖） */
    ADKEY_EVENT_UP,            /**< 抬起 */
    ADKEY_EVENT_CLICK,         /**< 单击（抬起且短于长按阈值） */
    ADKEY_EVENT_LONG_PRESS,    /**< 长按达到阈值 */
} adkey_event_t;

typedef void (*adkey_cb_t)(adkey_id_t id, adkey_event_t ev, void *user_data);

/** 初始化：ADC 采样由 adc_bus 提供（共享 ADC1 单元，通道 CH9），本函数自建扫描任务 */
esp_err_t adkey_init(void);

/** 注册回调（只保留最后一个注册者，和 key_register_cb 约定一致） */
esp_err_t adkey_register_cb(adkey_cb_t cb, void *user_data);

/** 某键当前是否处于按下状态 */
bool adkey_is_pressed(adkey_id_t id);

/** 读当前 ADC 电压（mV），供串口调试台显示 */
int adkey_raw_mv(void);

/** 最近一次"认定按下"时的 ADC 读数（mV），没按过返回 -1。
 *  事件回调里打印触发值要用它（adkey_raw_mv() 是实时值，回调时已回到空闲） */
int adkey_last_mv(void);

#ifdef __cplusplus
}
#endif
