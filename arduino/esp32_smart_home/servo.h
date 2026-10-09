#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// ======================================================================
// 舵机：窗户和门，窗帘那路写死停用
//
// 对应 ESP-IDF 工程的 components/BSP/SERVO/servo.c。
// 三路共用一套 50Hz PWM，到位后自动松劲，免得一直使劲嗡嗡响。
// ======================================================================

// 三路舵机编号，窗帘那路停用了也留着位子，索引别乱
typedef enum {
    // 窗帘那路，已写死停用
    SERVO_CURTAIN = 0,
    // 窗户那路
    SERVO_WINDOW,
    // 门那路
    SERVO_DOOR,
    SERVO_MAX,
} servo_id_t;

// 把硬件准备好，三路先归到关位
bool servo_init(void);

// 马上转到指定角度
bool servo_set_angle(servo_id_t id, float deg);

// 零是全关百是全开，窗户门按各自的关位开位算
bool servo_set_percent(servo_id_t id, uint8_t percent);

// 查现在在哪角度
float servo_get_angle(servo_id_t id);

// 查要去的角度
float servo_get_target(servo_id_t id);

// 松劲省电防抖
bool servo_detach(servo_id_t id);

// 重新使劲
bool servo_attach(servo_id_t id);

// 舵机号换名字
const char *servo_name(servo_id_t id);

// 这路舵机接没接，写死停用的给 false
bool servo_is_enabled(servo_id_t id);

// 主循环喊这个，到点自动松劲
void servo_poll(void);
