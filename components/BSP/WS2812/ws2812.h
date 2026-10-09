#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// 翻转引脚直接发
esp_err_t ws2812_bitbang_write(gpio_num_t gpio, const uint8_t *grb, uint32_t len);

#ifdef __cplusplus
}
#endif
