/**
 * @file  voice_internal.h
 * @brief 语音模块【内部】接口 —— 只给 components/BSP/VOICE/ 下的实现文件用
 *
 * 为什么要有这个文件：
 *   voice.c 里的 voice_dispatch() 是"所有语音识别来源的唯一汇合点"：
 *       ASRPRO UART 解析    ─┐
 *                            ├─→ voice_dispatch() → App 层回调（main.c::voice_on_cmd）
 *       ESP-SR 识别结果     ─┘
 *   ESP-SR 实现在同目录的 voice_esp_sr.c 里，它必须能调到 voice_dispatch()，
 *   但【不希望】把这个函数暴露到公共头文件 voice.h 里 —— 一旦公开，
 *   App 层就可能绕过 voice_inject_cmd() 直接乱调，破坏"两条来源、一个出口"
 *   这个设计约束。
 *   所以用"内部头文件"把口子只开给同目录的实现文件（C 里没有 package 概念，
 *   靠命名和注释约定：外部代码请只 include voice.h）。
 */
#pragma once

#include "voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行一条语音指令：打日志 + 调注册的回调
 *
 * @note  ⚠ 回调运行在【调用者任务】上下文：
 *          · 来自 ESP-SR → 跑在 voice_sr 任务里；
 *          · 来自 ASRPRO → 跑在 voice_rx 任务里；
 *          · 来自 voice_inject_cmd() → 跑在调用者任务里。
 *        回调里不要做阻塞操作，耗时动作请转投业务队列。
 */
void voice_dispatch(voice_cmd_t cmd);

#ifdef __cplusplus
}
#endif
