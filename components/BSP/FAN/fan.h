/*
 * 模块：
 *   风扇的对外接口。管开停和风速，被 device_model.c 调用，
 *   向下用 LEDC 出高频波形推风扇管子，频率高到听不见吱吱声。
 *
 * 功能：
 *   开关风扇
 *   调风速百分比
 *   可选读转速
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：把硬件准备好 */
esp_err_t fan_init(void);

/* 功能：设风速百分比 */
esp_err_t fan_set_speed(uint8_t percent);

uint8_t fan_get_speed(void);

esp_err_t fan_off(void);

/* 功能：开就满速关就停 */
esp_err_t fan_set_power(bool on);

bool fan_get_power(void);

/* 功能：读转速没接就零 */
uint32_t fan_get_rpm(void);

#ifdef __cplusplus
}
#endif
