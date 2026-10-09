#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把硬件准备好
esp_err_t fan_init(void);

// 设风速百分比
esp_err_t fan_set_speed(uint8_t percent);

uint8_t fan_get_speed(void);

esp_err_t fan_off(void);

// 开就满速关就停
esp_err_t fan_set_power(bool on);

bool fan_get_power(void);

#ifdef __cplusplus
}
#endif
