/**
 * @file  fan.h
 * @brief 风扇调速（MOS 管 PWM，GPIO18，LEDC 25kHz）
 *
 * 为什么用 25kHz：超出人耳听觉范围，风扇不会发出"吱吱"的 PWM 啸叫。
 * 测速 TACH（GPIO14）是可选功能，板子没接也能正常工作。
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化风扇 PWM（幂等）。初始为停转 */
esp_err_t fan_init(void);

/** @brief 设定转速百分比 0~100。0 会真正输出 0 占空比（完全停转） */
esp_err_t fan_set_speed(uint8_t percent);

uint8_t fan_get_speed(void);

esp_err_t fan_off(void);

/** @brief 兼容旧接口：true = 全速，false = 停 */
esp_err_t fan_set_power(bool on);

bool fan_get_power(void);

/**
 * @brief 读转速（RPM）
 * @return 未启用/未接测速线时返回 0
 */
uint32_t fan_get_rpm(void);

#ifdef __cplusplus
}
#endif
