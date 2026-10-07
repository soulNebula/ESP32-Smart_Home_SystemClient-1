#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 起单元和校准
esp_err_t adc_bus_init(void);

// 读原始值 0到4095
esp_err_t adc_bus_read_raw(adc_channel_t ch, int *out_raw);

// 读电压毫伏
esp_err_t adc_bus_read_mv(adc_channel_t ch, int *out_mv);

// 多读几次取平均
int adc_bus_read_mv_avg(adc_channel_t ch, int samples);

#ifdef __cplusplus
}
#endif
