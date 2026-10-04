/**
 * @file  adc_bus.h
 * @brief ADC1 单次采样 + 校准 —— 光敏电阻（GPIO1）/ 雨滴传感器（GPIO2）
 *
 * ESP32-S3 开 WiFi 后 ADC2 不可用，所以本工程只用 ADC1。
 */
#pragma once

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 ADC1 单次采样单元 + 曲线校准（幂等） */
esp_err_t adc_bus_init(void);

/** @brief 读原始值 0~4095（12bit） */
esp_err_t adc_bus_read_raw(adc_channel_t ch, int *out_raw);

/** @brief 读校准后的电压，单位 mV */
esp_err_t adc_bus_read_mv(adc_channel_t ch, int *out_mv);

/**
 * @brief 读多次取平均，抗抖动
 * @param samples 采样次数（建议 16~32，太多会拖慢任务）
 * @return 平均 mV；失败返回 -1
 */
int adc_bus_read_mv_avg(adc_channel_t ch, int samples);

#ifdef __cplusplus
}
#endif
