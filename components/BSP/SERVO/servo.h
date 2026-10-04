/**
 * @file  servo.h
 * @brief 舵机驱动（窗帘 GPIO15 / 窗户 GPIO16 / 门 GPIO17）
 *
 * 用 LEDC 硬件 PWM 产生 50Hz、500~2500us 的舵机信号。
 * 三路共用 LEDC_TIMER_0（50Hz 必须一致），占 3 个通道。
 *
 * 注意：舵机是「位置伺服」，上电后如果持续给信号会一直保持力矩、
 *       发热且耗电。长时间不动时建议调用 servo_detach() 松劲。
 *       ★ 本驱动【不会】自动松劲 —— 否则 servo_set_angle() 就得阻塞等待，
 *         而它会被按键/语音/MQTT 回调直接调用。需要松劲请显式调 servo_detach()。
 *
 * 另注：servo_set_pulse_us() 是纯调试接口，它只改 PWM 脉宽，
 *       不会同步 servo_get_angle() / servo_get_target() 的返回值。
 */
#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVO_CURTAIN = 0,  /**< 窗帘 GPIO15 */
    SERVO_WINDOW,       /**< 窗户 GPIO16 */
    SERVO_DOOR,         /**< 门   GPIO17 */
    SERVO_MAX,
} servo_id_t;

/** @brief 初始化 3 路舵机 PWM（幂等）。会先把所有舵机归到「关闭/复位」角度 */
esp_err_t servo_init(void);

/**
 * @brief 立即转到指定角度
 * @param deg 0~180 度，越界自动钳位
 */
esp_err_t servo_set_angle(servo_id_t id, float deg);

/**
 * @brief 平滑转动（分步插值），避免舵机"哐"一下甩过去
 * @param ms 总时长；0 等价于立即。内部用 esp_timer 阻塞式延时
 */
esp_err_t servo_set_angle_smooth(servo_id_t id, float deg, uint32_t ms);

/** @brief 按百分比控制：0 = 全关，100 = 全开（内部线性映射到 0~180°） */
esp_err_t servo_set_percent(servo_id_t id, uint8_t percent);

/** @brief 当前实际角度（最后设定的值） */
float servo_get_angle(servo_id_t id);

/** @brief 目标角度（平滑转动过程中与 servo_get_angle 不同） */
float servo_get_target(servo_id_t id);

/** @brief 直接给脉宽（调试用），单位 us */
esp_err_t servo_set_pulse_us(servo_id_t id, uint32_t us);

/** @brief 停止输出 PWM，舵机松劲（省电、防抖动） */
esp_err_t servo_detach(servo_id_t id);

/** @brief 恢复 PWM 输出 */
esp_err_t servo_attach(servo_id_t id);

/** @brief 名称，用于日志 / MQTT / OLED："curtain" / "window" / "door" */
const char *servo_name(servo_id_t id);

/** @brief 由名字反查，找不到返回 SERVO_MAX */
servo_id_t servo_from_name(const char *name);

#ifdef __cplusplus
}
#endif
