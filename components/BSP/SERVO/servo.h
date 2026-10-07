#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 三路舵机编号
typedef enum {
    // 窗帘那路
    SERVO_CURTAIN = 0,
    // 窗户那路
    SERVO_WINDOW,
    // 门那路
    SERVO_DOOR,
    SERVO_MAX,
} servo_id_t;

// 把硬件准备好
esp_err_t servo_init(void);

// 马上转到指定角度
esp_err_t servo_set_angle(servo_id_t id, float deg);

// 慢慢转别猛甩过去
// 会卡住当前任务一会
esp_err_t servo_set_angle_smooth(servo_id_t id, float deg, uint32_t ms);

// 零是全关百是全开
esp_err_t servo_set_percent(servo_id_t id, uint8_t percent);

// 查现在在哪角度
float servo_get_angle(servo_id_t id);

// 查要去的角度
float servo_get_target(servo_id_t id);

// 直接给脉宽调试
esp_err_t servo_set_pulse_us(servo_id_t id, uint32_t us);

// 松劲省电防抖
esp_err_t servo_detach(servo_id_t id);

// 重新使劲
esp_err_t servo_attach(servo_id_t id);

// 舵机号换名字
const char *servo_name(servo_id_t id);

// 名字反查舵机号
servo_id_t servo_from_name(const char *name);

#ifdef __cplusplus
}
#endif
