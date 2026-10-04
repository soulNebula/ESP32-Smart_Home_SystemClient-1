/**
 * @file  servo.c
 * @brief 舵机驱动实现（窗帘 GPIO15 / 窗户 GPIO16 / 门 GPIO17）
 *
 * 【实现要点】
 *   1) 用 LEDC 硬件 PWM 产生 50Hz 舵机信号；三路【共用】BSP_SERVO_TIMER，
 *      因为一个 LEDC 定时器只有一个频率，而三路舵机必须都是 50Hz。
 *   2) 14bit @ 50Hz：满量程 = 2^14 = 16384，周期 = 20000us。
 *        duty = pulse_us * 16384 / 20000      （1 个计数 ≈ 1.22us）
 *   3) 角度 → 脉宽：pulse = MIN + (MAX - MIN) * deg / 180
 *   4) detach = 把该通道 duty 置 0（输出恒低 = 没有 PWM 脉冲），舵机失去力矩而松劲。
 *      这里【不】用 ledc_stop()，因为 ledc_stop() 会把 GPIO 从 LEDC 矩阵里摘掉，
 *      重新 attach 就得重跑 ledc_channel_config()；而 duty=0 效果一样且切换更快。
 *   5) 平滑转动：按理想时间轴（esp_timer_get_time）分步插值，每步 ~1°，
 *      步间用 vTaskDelay 让出 CPU（不足 1 tick 才用 esp_rom_delay_us 补），
 *      总时长≈ms 且误差不累积。
 *
 * 所有引脚/参数都取自 board_config.h，本文件不硬编码任何 GPIO 号。
 */
#include "servo.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_assert.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SERVO";

/* 编译期自检：枚举数量必须和 board_config.h 对齐 */
ESP_STATIC_ASSERT(SERVO_MAX == BSP_SERVO_COUNT, "SERVO_MAX must match BSP_SERVO_COUNT");

#define SERVO_FULL_SCALE    16384U   /* 2^14，对应 BSP_SERVO_RES = LEDC_TIMER_14_BIT */
#define SERVO_PERIOD_US     20000U   /* 1 / 50Hz = 20000us */
#define SERVO_DEG_MIN       0.0f
#define SERVO_DEG_MAX       180.0f
#define SERVO_ANGLE_EPS     0.01f    /* 角度差小于此值就不再插值 */

/** 单路舵机的静态上下文（每个通道一份） */
typedef struct {
    ledc_channel_t channel;    /* LEDC 通道 */
    gpio_num_t     gpio;       /* 信号脚（来自 board_config.h） */
    float          angle;      /* 当前实际角度（平滑过程中 = 插值中间值） */
    float          target;     /* 目标角度（平滑过程中 = 最终目标） */
    uint32_t       last_duty;  /* 最近一次有效 duty，attach 时用它恢复 */
    bool           attached;   /* false = 已 detach（duty 0，舵机松劲） */
    esp_timer_handle_t release_timer;  /* 到位后自动松劲的一次性定时器 */
} servo_ctx_t;

/* 顺序必须和 servo_id_t 一致：CURTAIN / WINDOW / DOOR */
static servo_ctx_t s_servo[SERVO_MAX] = {
    { BSP_SERVO_CH_CURTAIN, BSP_SERVO_GPIO_CURTAIN, 0.0f, 0.0f, 0U, false, NULL },
    { BSP_SERVO_CH_WINDOW,  BSP_SERVO_GPIO_WINDOW,  0.0f, 0.0f, 0U, false, NULL },
    { BSP_SERVO_CH_DOOR,    BSP_SERVO_GPIO_DOOR,    0.0f, 0.0f, 0U, false, NULL },
};

static const char *const s_servo_name[SERVO_MAX] = { "curtain", "window", "door" };

static bool s_inited = false;

/* ------------------------------------------------------------------ */
/*  内部小工具                                                         */
/* ------------------------------------------------------------------ */

static bool servo_id_valid(servo_id_t id)
{
    return ((int)id >= 0) && (id < SERVO_MAX);
}

static float servo_clamp_deg(float deg)
{
    if (deg < SERVO_DEG_MIN) {
        return SERVO_DEG_MIN;
    }
    if (deg > SERVO_DEG_MAX) {
        return SERVO_DEG_MAX;
    }
    return deg;
}

/** 角度 → 脉宽(us)。输入先钳位到 0~180 */
static uint32_t servo_angle_to_pulse_us(float deg)
{
    const float span = (float)(BSP_SERVO_MAX_PULSE_US - BSP_SERVO_MIN_PULSE_US);
    const float pulse = (float)BSP_SERVO_MIN_PULSE_US + span * servo_clamp_deg(deg) / 180.0f;
    /* +0.5 做四舍五入，比直接截断更接近目标脉宽 */
    return (uint32_t)(pulse + 0.5f);
}

/** 脉宽(us) → duty（14bit） */
static uint32_t servo_pulse_to_duty(uint32_t pulse_us)
{
    return (uint32_t)((uint64_t)pulse_us * (uint64_t)SERVO_FULL_SCALE / (uint64_t)SERVO_PERIOD_US);
}

static uint32_t servo_angle_to_duty(float deg)
{
    return servo_pulse_to_duty(servo_angle_to_pulse_us(deg));
}

/**
 * @brief 真正写硬件：ledc_set_duty + ledc_update_duty
 * @param remember true 时把 duty 记为 last_duty（detach 后 attach 用）
 */
static esp_err_t servo_write_duty(int idx, uint32_t duty, bool remember)
{
    esp_err_t err = ledc_set_duty(BSP_SERVO_MODE, s_servo[idx].channel, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ledc_set_duty(duty=%u) failed: %s",
                 s_servo_name[idx], (unsigned)duty, esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(BSP_SERVO_MODE, s_servo[idx].channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ledc_update_duty failed: %s", s_servo_name[idx], esp_err_to_name(err));
        return err;
    }

    if (remember) {
        s_servo[idx].last_duty = duty;
    }
    return ESP_OK;
}

/**
 * @brief 更新角度并输出（是否同时改 target 由调用者决定）
 * @note  若此前处于 detach 状态，这里会先重新挂上（attached = true）再输出脉宽
 */
static esp_err_t servo_apply(int idx, float deg, bool update_target)
{
    const float a = servo_clamp_deg(deg);

    s_servo[idx].angle = a;
    if (update_target) {
        s_servo[idx].target = a;
    }

    if (!s_servo[idx].attached) {
        ESP_LOGD(TAG, "%s: re-attach before output", s_servo_name[idx]);
        s_servo[idx].attached = true;
    }

    return servo_write_duty(idx, servo_angle_to_duty(a), true);
}

/** 松劲：duty = 0 即没有脉冲。不删通道，方便随时 attach */
static esp_err_t servo_do_detach(int idx)
{
    /* 手动松劲时把挂起的自动松劲定时器一并取消（重复松劲无害） */
    if (s_servo[idx].release_timer != NULL) {
        esp_timer_stop(s_servo[idx].release_timer);
    }
    esp_err_t err = servo_write_duty(idx, 0U, false);
    if (err == ESP_OK) {
        s_servo[idx].attached = false;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  到位后自动松劲                                                     */
/* ------------------------------------------------------------------ */
/* 为什么：舵机被命令到机械止点（0°/180°）时会顶着止点持续较劲，表现为
 * 高频抖动/嗡鸣；长时间保持力矩还费电。让舵机到位后 BSP_SERVO_RELEASE_MS
 * 毫秒自动松劲（无 PWM），与开机初始化的处理保持一致。轻负载场景
 * （窗帘/窗户/门）松劲后一般不会跑位；重负载会跑位的话把该宏改成 0 关闭。 */

static void servo_release_cb(void *arg)
{
    const int idx = (int)(intptr_t)arg;
    if (!servo_id_valid((servo_id_t)idx)) {
        return;
    }
    ESP_LOGI(TAG, "%s: auto release (idle %ums)",
             s_servo_name[idx], (unsigned)BSP_SERVO_RELEASE_MS);
    (void)servo_do_detach(idx);
}

/** 取消未触发的自动松劲（每次新动作前调用，避免松劲打断正在进行的转动） */
static void servo_cancel_release(int idx)
{
    if (s_servo[idx].release_timer != NULL) {
        esp_timer_stop(s_servo[idx].release_timer);
    }
}

/** 动作完成后(重新)排一个一次性定时器，到点自动松劲 */
static void servo_schedule_release(int idx)
{
#if BSP_SERVO_RELEASE_MS > 0
    if (s_servo[idx].release_timer == NULL) {
        return;   /* 定时器没建起来（初始化失败），静默退化成"一直保持" */
    }
    esp_timer_start_once(s_servo[idx].release_timer, (uint64_t)BSP_SERVO_RELEASE_MS * 1000);
#else
    (void)idx;
#endif
}

/** 忙等/让出 CPU 直到到达绝对时刻 due_us（esp_timer 时基） */
static void servo_wait_until(int64_t due_us)
{
    while (1) {
        const int64_t remain_us = due_us - esp_timer_get_time();
        if (remain_us <= 0) {
            return;
        }

        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
        if (ticks == 0) {
            /* 不足一个 tick（本工程 CONFIG_FREERTOS_HZ=1000 → 1ms）：
             * 用微秒级忙等等完，绝不长时间霸占 CPU */
            esp_rom_delay_us((uint32_t)remain_us);
            return;
        }

        vTaskDelay(ticks);   /* 让出 CPU，别死转 */
    }
}

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t servo_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    /* ---- 1. 定时器：50Hz / 14bit，三路共用 ---- */
    ledc_timer_config_t tcfg = {
        .speed_mode      = BSP_SERVO_MODE,
        .duty_resolution = BSP_SERVO_RES,
        .timer_num       = BSP_SERVO_TIMER,
        .freq_hz         = BSP_SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 2. 三个通道 ---- */
    for (int i = 0; i < (int)SERVO_MAX; i++) {
        ledc_channel_config_t ccfg = {
            .gpio_num   = s_servo[i].gpio,
            .speed_mode = BSP_SERVO_MODE,
            .channel    = s_servo[i].channel,
            .intr_type  = LEDC_INTR_DISABLE,      /* 不用硬件渐变中断 */
            .timer_sel  = BSP_SERVO_TIMER,
            .duty       = 0,                      /* 先给 0，下面按"关闭位"再设 */
            .hpoint     = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags      = { .output_invert = 0 },
        };

        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: ledc_channel_config(ch=%d gpio=%d) failed: %s",
                     s_servo_name[i], (int)s_servo[i].channel, (int)s_servo[i].gpio,
                     esp_err_to_name(err));
            return err;   /* 通道建不起来后面全是错的，直接返回 */
        }

        /* ---- 3. 初始角度 = 关闭位（0°），随后 detach 省电 ---- */
        s_servo[i].angle    = 0.0f;
        s_servo[i].target   = 0.0f;
        s_servo[i].attached = true;   /* 先允许下面写 duty */

        err = servo_write_duty(i, servo_angle_to_duty(0.0f), true);
        if (err != ESP_OK) {
            return err;
        }

        err = servo_do_detach(i);
        if (err != ESP_OK) {
            return err;
        }

        ESP_LOGI(TAG, "  %-7s ch=%d gpio=%-2d -> 0 deg (closed), detached",
                 s_servo_name[i], (int)s_servo[i].channel, (int)s_servo[i].gpio);

        /* ---- 4. 到位后自动松劲的一次性定时器（失败只告警，退化为一直保持） ---- */
#if BSP_SERVO_RELEASE_MS > 0
        const esp_timer_create_args_t targs = {
            .callback        = servo_release_cb,
            .arg             = (void *)(intptr_t)i,
            .name            = s_servo_name[i],
            .dispatch_method = ESP_TIMER_TASK,
        };
        if (esp_timer_create(&targs, &s_servo[i].release_timer) != ESP_OK) {
            ESP_LOGW(TAG, "%s: release timer create failed, auto-release disabled",
                     s_servo_name[i]);
            s_servo[i].release_timer = NULL;
        }
#endif
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: %dHz / %d bit, pulse %u~%uus, full scale %u",
             BSP_SERVO_FREQ_HZ, (int)BSP_SERVO_RES,
             (unsigned)BSP_SERVO_MIN_PULSE_US, (unsigned)BSP_SERVO_MAX_PULSE_US,
             (unsigned)SERVO_FULL_SCALE);
    return ESP_OK;
}

esp_err_t servo_set_angle(servo_id_t id, float deg)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int idx = (int)id;
    const float a = servo_clamp_deg(deg);

    servo_cancel_release(idx);              /* 新动作取消上一次挂起的自动松劲 */
    esp_err_t err = servo_apply(idx, a, true);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s -> %.1f deg (pulse %uus, duty %u)",
             s_servo_name[idx], a, (unsigned)servo_angle_to_pulse_us(a),
             (unsigned)servo_angle_to_duty(a));
    servo_schedule_release(idx);            /* 到位后自动松劲，防止顶止点抖动 */
    return ESP_OK;
}

esp_err_t servo_set_angle_smooth(servo_id_t id, float deg, uint32_t ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int   idx    = (int)id;
    const float target = servo_clamp_deg(deg);

    if (ms == 0) {
        return servo_set_angle(id, target);
    }

    servo_cancel_release(idx);              /* 新动作取消上一次挂起的自动松劲 */

    /* 防呆：ms * configTICK_RATE_HZ 在 TickType_t(uint32) 里会溢出，60s 已经远超实际需要 */
    if (ms > 60000U) {
        ESP_LOGW(TAG, "%s smooth %ums too long, clamp to 60000ms", s_servo_name[(int)id], (unsigned)ms);
        ms = 60000U;
    }

    const float start = s_servo[idx].angle;
    const float diff  = target - start;

    /* 目标先记下来：平滑过程中 servo_get_target() 立刻返回最终值 */
    s_servo[idx].target = target;

    if (fabsf(diff) < SERVO_ANGLE_EPS) {
        return servo_apply(idx, target, false);
    }

    /* 每步约 1°，但步数不超过 ms 对应的 tick 数：
     * 这样每步至少能睡 1 个 tick（既不会忙等霸占 CPU，也不会把总时长拖长） */
    TickType_t total_ticks = pdMS_TO_TICKS(ms);
    if (total_ticks < 1) {
        total_ticks = 1;
    }

    int steps = (int)ceilf(fabsf(diff));
    if (steps < 1) {
        steps = 1;
    }
    if ((uint32_t)steps > (uint32_t)total_ticks) {
        steps = (int)total_ticks;
    }

    const int64_t t0       = esp_timer_get_time();
    const int64_t total_us = (int64_t)ms * 1000;

    ESP_LOGI(TAG, "%s smooth %.1f -> %.1f deg in %ums (%d steps)",
             s_servo_name[idx], start, target, (unsigned)ms, steps);

    for (int i = 1; i <= steps; i++) {
        const float a = start + diff * (float)i / (float)steps;

        esp_err_t err = servo_apply(idx, a, false);   /* 只改当前角度，不动 target */
        if (err != ESP_OK) {
            return err;
        }

        /* 按绝对时间轴走，vTaskDelay 的取整误差不会累积 */
        servo_wait_until(t0 + total_us * (int64_t)i / (int64_t)steps);
    }

    /* 收尾：保证精确停在目标角度（浮点插值可能有极小的余差） */
    esp_err_t err = servo_apply(idx, target, false);
    if (err == ESP_OK) {
        servo_schedule_release(idx);        /* 到位后自动松劲 */
    }
    return err;
}

esp_err_t servo_set_percent(servo_id_t id, uint8_t percent)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    /* 0 -> 0°（全关），100 -> 180°（全开） */
    return servo_set_angle(id, (float)percent * 180.0f / 100.0f);
}

float servo_get_angle(servo_id_t id)
{
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    return s_servo[id].angle;   /* 平滑过程中 = 当前插值角度 */
}

float servo_get_target(servo_id_t id)
{
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    return s_servo[id].target;  /* 平滑过程中 = 最终目标 */
}

esp_err_t servo_set_pulse_us(servo_id_t id, uint32_t us)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int idx = (int)id;

    /* 钳位到 [MIN, MAX]，避免给舵机灌非法脉宽把它顶死 */
    if (us < (uint32_t)BSP_SERVO_MIN_PULSE_US) {
        us = (uint32_t)BSP_SERVO_MIN_PULSE_US;
    }
    if (us > (uint32_t)BSP_SERVO_MAX_PULSE_US) {
        us = (uint32_t)BSP_SERVO_MAX_PULSE_US;
    }

    /* 调试接口：直接给脉宽，不去猜角度，所以不改 angle/target */
    servo_cancel_release(idx);
    if (!s_servo[idx].attached) {
        s_servo[idx].attached = true;
    }

    ESP_LOGD(TAG, "%s set pulse %uus (duty %u)", s_servo_name[idx],
             (unsigned)us, (unsigned)servo_pulse_to_duty(us));
    esp_err_t err = servo_write_duty(idx, servo_pulse_to_duty(us), true);
    if (err == ESP_OK) {
        servo_schedule_release(idx);
    }
    return err;
}

esp_err_t servo_detach(servo_id_t id)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = servo_do_detach((int)id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s detached (no pulse, loose)", s_servo_name[id]);
    }
    return err;
}

esp_err_t servo_attach(servo_id_t id)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int idx = (int)id;
    if (s_servo[idx].attached) {
        return ESP_OK;   /* 已经是挂上的状态，幂等 */
    }

    /* 恢复上次设定的 duty */
    esp_err_t err = servo_write_duty(idx, s_servo[idx].last_duty, false);
    if (err == ESP_OK) {
        s_servo[idx].attached = true;
        ESP_LOGI(TAG, "%s attached (restore duty %u)", s_servo_name[idx],
                 (unsigned)s_servo[idx].last_duty);
    }
    return err;
}

const char *servo_name(servo_id_t id)
{
    return servo_id_valid(id) ? s_servo_name[id] : "unknown";
}

servo_id_t servo_from_name(const char *name)
{
    if (name == NULL) {
        return SERVO_MAX;
    }
    for (int i = 0; i < (int)SERVO_MAX; i++) {
        if (strcmp(name, s_servo_name[i]) == 0) {
            return (servo_id_t)i;
        }
    }
    return SERVO_MAX;
}
