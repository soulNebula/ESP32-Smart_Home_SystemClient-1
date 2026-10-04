/*
 * 模块：
 *   麦克风采音。INMP441 走 I2S 三根线，不是 I2C，接错线只会一直没声音、
 *   不会报错；采到的声音给 voice_esp_sr.c 喂语音识别，被 board.c 开机自检
 *   和 main.c 的串口台调用，自己向下用 IDF 的 I2S 驱动。
 *   引脚和参数都取自 board_config.h。
 *
 * 功能：
 *   把麦克风通道打开
 *   连着读一段采样
 *   读最近的电平
 *   判断麦克风在不在
 *   采一段打印统计
 *   读两个字才得一个采样
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：最大能到多少 */
#define I2S_MIC_FULL_SCALE      32767

/* 功能：麦克风的电平快照 */
typedef struct {
    int   rms;          /* 功能：声音大小，有效值 */
    int   peak;         /* 功能：最大的那一下 */
    int   peak_to_peak; /* 功能：最高最低差多少 */
    int   dc;           /* 功能：平均偏置，理想近零 */
    int   db_x10;       /* 功能：音量折成分贝 */
    size_t   last_samples;  /* 功能：这次读到几个点 */
    uint32_t read_count;    /* 功能：一共读了多少次 */
    uint32_t clip;          /* 功能：削顶了几个点 */
} i2s_mic_level_t;

/* 功能：把麦克风通道打开 */
esp_err_t i2s_mic_init(void);

/* 功能：连着读够这么多点 */
esp_err_t i2s_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms);

/* 功能：读最近一次的电平 */
void i2s_mic_read_level(i2s_mic_level_t *out);

/* 功能：看麦克风有没有在响 */
bool i2s_mic_is_ready(void);

/* 功能：采一段打印统计 */
void i2s_mic_dump(int seconds);

#ifdef __cplusplus
}
#endif
