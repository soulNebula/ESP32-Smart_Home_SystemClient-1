/*
 * 模块：
 *   ADC 采集的对外口子。光敏和雨滴两路都从这儿读，
 *   被 sensor.c 和 adkey.c 调用，具体实现在 adc_bus.c，
 *   引脚在 board_config.h 里。
 *
 * 功能：
 *   起采样单元
 *   读原始值
 *   读电压毫伏
 *   多读几次取平均
 */
#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：起单元和校准 */
esp_err_t adc_bus_init(void);

/* 功能：读原始值 0到4095 */
esp_err_t adc_bus_read_raw(adc_channel_t ch, int *out_raw);

/* 功能：读电压毫伏 */
esp_err_t adc_bus_read_mv(adc_channel_t ch, int *out_mv);

/* 功能：多读几次取平均 */
int adc_bus_read_mv_avg(adc_channel_t ch, int samples);

#ifdef __cplusplus
}
#endif
