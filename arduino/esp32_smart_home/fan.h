#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// ======================================================================
// 风扇：低边 MOS 管 + PWM 调速
//
// 对应 ESP-IDF 工程的 components/BSP/FAN/fan.c。
// 没买 MOS 管的话，用一颗 NPN 三极管（S8050/SS8050/2N2222/S9013）
// 当低边开关一样能跑，接法见 arduino/README.md。
// ======================================================================

// 把硬件准备好，开机先停转
bool fan_init(void);

// 按百分比调风速，0 就彻底停
bool fan_set_speed(uint8_t percent);

// 查现在多大风
uint8_t fan_get_speed(void);

// 直接把风扇关掉
bool fan_off(void);

// 只认开和关，开就满速
bool fan_set_power(bool on);

// 查风扇转没转
bool fan_get_power(void);
