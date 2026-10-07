#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把识别跑起来
esp_err_t voice_esp_sr_start(void);

// 看识别起来没
bool voice_esp_sr_is_ready(void);

// 报要喊那句话
const char *voice_esp_sr_wake_word(void);

// 报命令词模型
const char *voice_esp_sr_mn_model(void);

// 打一张命令表
void voice_esp_sr_print_commands(void);

// 看在不在等命令
bool voice_esp_sr_is_awake(void);

// 给屏幕留一句话
typedef struct {
    // 0 没新的 1 有新的
    volatile int pending;
    // 提示文字
    char         text[48];
} voice_ui_note_t;

extern voice_ui_note_t g_voice_ui_note;

// 写一条屏幕提示
void voice_ui_note(const char *text);

#ifdef __cplusplus
}
#endif
