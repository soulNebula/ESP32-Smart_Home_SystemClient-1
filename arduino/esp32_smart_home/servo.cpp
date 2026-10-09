#include "servo.h"

#include "pwm_compat.h"

// ======================================================================
// 舵机：窗户和门，窗帘那路写死停用
//
// 对应 ESP-IDF 工程的 components/BSP/SERVO/servo.c。
// 三路共用一套 50Hz PWM；到位后隔 BSP_SERVO_RELEASE_MS 自动松劲，
// 免得舵机一直顶着劲儿嗡嗡响、还费电。
// ======================================================================

// 满量程按十四位算，这只是个换算比例，不是占空比上限
#define SERVO_FULL_SCALE    16384u
// 一圈两万微秒，50Hz
#define SERVO_PERIOD_US     20000u
#define SERVO_DEG_MIN       0.0f
#define SERVO_DEG_MAX       180.0f

// 换算前提是十四位，改了要报错
static_assert(BSP_SERVO_RES_BITS == 14, "SERVO_FULL_SCALE assumes 14-bit resolution");

// 存一路舵机的状态
typedef struct {
    // 出波形的通道
    uint8_t channel;
    // 信号脚
    uint8_t gpio;
    // 现在在哪角度
    float   angle;
    // 要去的角度
    float   target;
    // 上次的占空比
    uint32_t last_duty;
    // 现在使劲还是松劲
    bool    attached;
    // 到点松劲的时刻，0 表示没排
    uint32_t release_at;
} servo_ctx_t;

// 窗帘窗户门三路的状态
static servo_ctx_t s_servo[SERVO_MAX] = {
    { BSP_SERVO_CH_CURTAIN, BSP_SERVO_GPIO_CURTAIN, 0.0f, 0.0f, 0u, false, 0u },
    { BSP_SERVO_CH_WINDOW,  BSP_SERVO_GPIO_WINDOW,  0.0f, 0.0f, 0u, false, 0u },
    { BSP_SERVO_CH_DOOR,    BSP_SERVO_GPIO_DOOR,    0.0f, 0.0f, 0u, false, 0u },
};

// 三路舵机的名字
static const char *const s_servo_name[SERVO_MAX] = { "curtain", "window", "door" };

static bool s_inited = false;

// 判断舵机号合不合法
static bool servo_id_valid(servo_id_t id) {
    return ((int)id >= 0) && (id < SERVO_MAX);
}

// 这路舵机是不是写死停用了
static bool servo_is_disabled(servo_id_t id) {
#if !BSP_SERVO_CURTAIN_ENABLE
    // 窗帘那路不要了
    return (id == SERVO_CURTAIN);
#else
    (void)id;
    return false;
#endif
}

// 把角度卡在范围内
static float servo_clamp_deg(float deg) {
    if (deg < SERVO_DEG_MIN) {
        return SERVO_DEG_MIN;
    }
    if (deg > SERVO_DEG_MAX) {
        return SERVO_DEG_MAX;
    }
    return deg;
}

// 这路舵机的关位角度：窗户和门两头都留了余量
static float servo_closed_deg(servo_id_t id) {
    switch (id) {
    case SERVO_WINDOW:
    case SERVO_DOOR:
        return (float)BSP_SERVO_WINDOW_DOOR_CLOSED_DEG;
    default:
        // 窗帘那路停用了，用不上
        return SERVO_DEG_MIN;
    }
}

// 这路舵机的开位角度
static float servo_open_deg(servo_id_t id) {
    switch (id) {
    case SERVO_WINDOW:
    case SERVO_DOOR:
        return (float)BSP_SERVO_WINDOW_DOOR_OPEN_DEG;
    default:
        return SERVO_DEG_MAX;
    }
}

// 角度换成脉宽
static uint32_t servo_angle_to_pulse_us(float deg) {
    const float span  = (float)(BSP_SERVO_MAX_PULSE_US - BSP_SERVO_MIN_PULSE_US);
    const float pulse = (float)BSP_SERVO_MIN_PULSE_US + span * servo_clamp_deg(deg) / 180.0f;

    // 加半点算四舍五入
    return (uint32_t)(pulse + 0.5f);
}

// 脉宽换成占空比
static uint32_t servo_pulse_to_duty(uint32_t pulse_us) {
    return (uint32_t)(((uint64_t)pulse_us * SERVO_FULL_SCALE) / SERVO_PERIOD_US);
}

// 角度直接换成占空比
static uint32_t servo_angle_to_duty(float deg) {
    return servo_pulse_to_duty(servo_angle_to_pulse_us(deg));
}

// 把占空比写到硬件
static void servo_write_duty(int idx, uint32_t duty, bool remember) {
    // 停用的那路没挂 PWM，写了也没用
    if (servo_is_disabled((servo_id_t)idx)) {
        return;
    }

    pwm_write(s_servo[idx].channel, s_servo[idx].gpio, duty);

    if (remember) {
        s_servo[idx].last_duty = duty;
    }
}

// 改角度并输出
static void servo_apply(int idx, float deg, bool update_target) {
    const float a = servo_clamp_deg(deg);

    s_servo[idx].angle = a;
    if (update_target) {
        s_servo[idx].target = a;
    }

    s_servo[idx].attached = true;
    servo_write_duty(idx, servo_angle_to_duty(a), true);
}

// 松劲不再使劲
static void servo_do_detach(int idx) {
    s_servo[idx].release_at = 0u;

    // 脉宽给零就是不出波形，舵机自己就松了
    servo_write_duty(idx, 0u, false);
    s_servo[idx].attached = false;
}

// 排一个到点松劲
static void servo_schedule_release(int idx) {
#if BSP_SERVO_RELEASE_MS > 0
    s_servo[idx].release_at = millis() + (uint32_t)BSP_SERVO_RELEASE_MS;
#else
    // 关掉自动松劲就一直使劲
    (void)idx;
#endif
}

// 把硬件准备好
bool servo_init(void) {
    if (s_inited) {
        Serial.println("SERVO: already initialized");
        return true;
    }

    for (int i = 0; i < (int)SERVO_MAX; i++) {
        // 写死停用的那路连 PWM 都不挂，脚上一点波形都没有
        if (servo_is_disabled((servo_id_t)i)) {
            Serial.printf("SERVO:   %-7s 已写死停用（gpio%d 不接、不出波形）\n",
                          s_servo_name[i], (int)s_servo[i].gpio);
            continue;
        }

        if (!pwm_attach(s_servo[i].channel, s_servo[i].gpio,
                        BSP_SERVO_FREQ_HZ, BSP_SERVO_RES_BITS)) {
            Serial.printf("SERVO: %s gpio%d PWM 挂不上\n", s_servo_name[i], s_servo[i].gpio);
            return false;
        }

        // 先归位再松劲，归的是关位
        const float closed = servo_closed_deg((servo_id_t)i);
        s_servo[i].angle    = closed;
        s_servo[i].target   = closed;
        s_servo[i].attached = true;

        servo_write_duty(i, servo_angle_to_duty(closed), true);

        // 归位这一下不给劲儿，等上层设成"关"的时候再真转到关位
        servo_do_detach(i);

        Serial.printf("SERVO:   %-7s ch=%d gpio=%-2d -> %.0f deg (closed), detached\n",
                      s_servo_name[i], (int)s_servo[i].channel, (int)s_servo[i].gpio,
                      (double)closed);
    }

    s_inited = true;
    Serial.printf("SERVO: init ok: %dHz / %d bit, pulse %u~%uus, full scale %u\n",
                  BSP_SERVO_FREQ_HZ, BSP_SERVO_RES_BITS,
                  (unsigned)BSP_SERVO_MIN_PULSE_US, (unsigned)BSP_SERVO_MAX_PULSE_US,
                  (unsigned)SERVO_FULL_SCALE);
    return true;
}

// 马上转到指定角度
bool servo_set_angle(servo_id_t id, float deg) {
    if (!s_inited || !servo_id_valid(id)) {
        return false;
    }

    // 停用的那路当没收到，省得上层以为转了
    if (servo_is_disabled(id)) {
        return true;
    }

    const int   idx = (int)id;
    const float a   = servo_clamp_deg(deg);

    servo_apply(idx, a, true);

    Serial.printf("SERVO: %s -> %.1f deg (pulse %uus, duty %u)\n",
                  s_servo_name[idx], (double)a,
                  (unsigned)servo_angle_to_pulse_us(a),
                  (unsigned)servo_angle_to_duty(a));

    // 到点松劲防抖
    servo_schedule_release(idx);
    return true;
}

// 按百分比转过去
bool servo_set_percent(servo_id_t id, uint8_t percent) {
    if (!s_inited || !servo_id_valid(id)) {
        return false;
    }

    if (percent > 100) {
        percent = 100;
    }

    // 零是全关百是全开，关位开位各按各的标定角度算
    const float closed = servo_closed_deg(id);
    const float open   = servo_open_deg(id);
    const float deg    = closed + (open - closed) * (float)percent / 100.0f;

    return servo_set_angle(id, deg);
}

// 查现在在哪角度
float servo_get_angle(servo_id_t id) {
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    return s_servo[id].angle;
}

// 查要去的角度
float servo_get_target(servo_id_t id) {
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    return s_servo[id].target;
}

// 松劲不再使劲
bool servo_detach(servo_id_t id) {
    if (!s_inited || !servo_id_valid(id)) {
        return false;
    }

    // 停用的那路没什么可松的
    if (servo_is_disabled(id)) {
        return true;
    }

    servo_do_detach((int)id);
    Serial.printf("SERVO: %s detached (no pulse, loose)\n", s_servo_name[id]);
    return true;
}

// 恢复使劲
bool servo_attach(servo_id_t id) {
    if (!s_inited || !servo_id_valid(id)) {
        return false;
    }

    // 停用的那路不恢复
    if (servo_is_disabled(id)) {
        return true;
    }

    const int idx = (int)id;
    if (s_servo[idx].attached) {
        // 本来就使劲直接返回
        return true;
    }

    // 按上次的占空比恢复
    servo_write_duty(idx, s_servo[idx].last_duty, false);
    s_servo[idx].attached = true;

    Serial.printf("SERVO: %s attached (restore duty %u)\n",
                  s_servo_name[idx], (unsigned)s_servo[idx].last_duty);
    return true;
}

// 舵机号换名字
const char *servo_name(servo_id_t id) {
    return servo_id_valid(id) ? s_servo_name[id] : "unknown";
}

// 这路舵机接没接：号合法又没写死停用才算接了
bool servo_is_enabled(servo_id_t id) {
    return servo_id_valid(id) && !servo_is_disabled(id);
}

// 主循环喊这个，到点自动松劲
void servo_poll(void) {
    if (!s_inited) {
        return;
    }

    for (int i = 0; i < (int)SERVO_MAX; i++) {
        if (!s_servo[i].attached || (s_servo[i].release_at == 0u)) {
            continue;
        }

        // 时间到就松劲，别一直顶着
        if ((int32_t)(millis() - s_servo[i].release_at) >= 0) {
            Serial.printf("SERVO: %s: auto release (idle %ums)\n",
                          s_servo_name[i], (unsigned)BSP_SERVO_RELEASE_MS);
            servo_do_detach(i);
        }
    }
}
