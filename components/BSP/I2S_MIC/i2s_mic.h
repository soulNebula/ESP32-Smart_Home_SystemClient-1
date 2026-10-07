#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 最大能到多少
#define I2S_MIC_FULL_SCALE      32767

// 麦克风的电平快照
typedef struct {
    // 声音大小，有效值
    int   rms;
    // 最大的那一下
    int   peak;
    // 最高最低差多少
    int   peak_to_peak;
    // 平均偏置，理想近零
    int   dc;
    // 音量折成分贝
    int   db_x10;
    // 这次读到几个点
    size_t   last_samples;
    // 一共读了多少次
    uint32_t read_count;
    // 削顶了几个点
    uint32_t clip;
} i2s_mic_level_t;

// 把麦克风通道打开
esp_err_t i2s_mic_init(void);

// 连着读够这么多点
esp_err_t i2s_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms);

// 读最近一次的电平
void i2s_mic_read_level(i2s_mic_level_t *out);

// 看麦克风有没有在响
bool i2s_mic_is_ready(void);

// 采一段打印统计
void i2s_mic_dump(int seconds);

#ifdef __cplusplus
}
#endif
