/*
 * 模块：
 *   舵机的对外接口。管窗帘、窗户、门三个位置，被 device_model.c 调用，
 *   向下用 LEDC 出五十赫兹的舵机信号。转到位会自动松劲，省电也防抖。
 *
 * 功能：
 *   转到指定角度
 *   可以慢慢转过去
 *   松劲省电防抖
 *   慢慢转会等等它
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：三路舵机编号 */
typedef enum {
    SERVO_CURTAIN = 0,  /* 功能：窗帘那路 */
    SERVO_WINDOW,       /* 功能：窗户那路 */
    SERVO_DOOR,         /* 功能：门那路 */
    SERVO_MAX,
} servo_id_t;

/* 功能：把硬件准备好 */
esp_err_t servo_init(void);

/* 功能：马上转到指定角度 */
esp_err_t servo_set_angle(servo_id_t id, float deg);

/* 功能：慢慢转别猛甩过去 */
/* 功能：会卡住当前任务一会 */
esp_err_t servo_set_angle_smooth(servo_id_t id, float deg, uint32_t ms);

/* 功能：零是全关百是全开 */
esp_err_t servo_set_percent(servo_id_t id, uint8_t percent);

/* 功能：查现在在哪角度 */
float servo_get_angle(servo_id_t id);

/* 功能：查要去的角度 */
float servo_get_target(servo_id_t id);

/* 功能：直接给脉宽调试 */
esp_err_t servo_set_pulse_us(servo_id_t id, uint32_t us);

/* 功能：松劲省电防抖 */
esp_err_t servo_detach(servo_id_t id);

/* 功能：重新使劲 */
esp_err_t servo_attach(servo_id_t id);

/* 功能：舵机号换名字 */
const char *servo_name(servo_id_t id);

/* 功能：名字反查舵机号 */
servo_id_t servo_from_name(const char *name);

#ifdef __cplusplus
}
#endif
