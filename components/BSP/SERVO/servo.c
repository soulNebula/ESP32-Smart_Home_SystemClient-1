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

// 核对舵机数量
ESP_STATIC_ASSERT(SERVO_MAX == BSP_SERVO_COUNT, "SERVO_MAX must match BSP_SERVO_COUNT");

// 满量程按十四位算
#define SERVO_FULL_SCALE    16384U
// 一圈两万微秒
#define SERVO_PERIOD_US     20000U
#define SERVO_DEG_MIN       0.0f
#define SERVO_DEG_MAX       180.0f
// 差太小就不再挪
#define SERVO_ANGLE_EPS     0.01f

// 存一路舵机的状态
typedef struct {
    // 出波形的通道
    ledc_channel_t channel;
    // 信号脚
    gpio_num_t     gpio;
    // 现在在哪角度
    float          angle;
    // 要去的角度
    float          target;
    // 上次的占空比
    uint32_t       last_duty;
    // 现在使劲还是松劲
    bool           attached;
    // 到点自动松劲
    esp_timer_handle_t release_timer;
} servo_ctx_t;

// 窗帘窗户门三路的状态
static servo_ctx_t s_servo[SERVO_MAX] = {
    { BSP_SERVO_CH_CURTAIN, BSP_SERVO_GPIO_CURTAIN, 0.0f, 0.0f, 0U, false, NULL },
    { BSP_SERVO_CH_WINDOW,  BSP_SERVO_GPIO_WINDOW,  0.0f, 0.0f, 0U, false, NULL },
    { BSP_SERVO_CH_DOOR,    BSP_SERVO_GPIO_DOOR,    0.0f, 0.0f, 0U, false, NULL },
};

// 三路舵机的名字
static const char *const s_servo_name[SERVO_MAX] = { "curtain", "window", "door" };

static bool s_inited = false;

// 判断舵机号合不合法
static bool servo_id_valid(servo_id_t id)
{
    return ((int)id >= 0) && (id < SERVO_MAX);
}

// 把角度卡在范围内
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

// 角度换成脉宽
static uint32_t servo_angle_to_pulse_us(float deg)
{
    const float span = (float)(BSP_SERVO_MAX_PULSE_US - BSP_SERVO_MIN_PULSE_US);
    const float pulse = (float)BSP_SERVO_MIN_PULSE_US + span * servo_clamp_deg(deg) / 180.0f;
    // 加半点算四舍五入
    return (uint32_t)(pulse + 0.5f);
}

// 脉宽换成占空比
static uint32_t servo_pulse_to_duty(uint32_t pulse_us)
{
    return (uint32_t)((uint64_t)pulse_us * (uint64_t)SERVO_FULL_SCALE / (uint64_t)SERVO_PERIOD_US);
}

// 角度直接换成占空比
static uint32_t servo_angle_to_duty(float deg)
{
    return servo_pulse_to_duty(servo_angle_to_pulse_us(deg));
}

// 把占空比写到硬件
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

// 改角度并输出
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

// 松劲不再使劲
static esp_err_t servo_do_detach(int idx)
{
    // 松劲顺手停定时器
    if (s_servo[idx].release_timer != NULL) {
        esp_timer_stop(s_servo[idx].release_timer);
    }
    esp_err_t err = servo_write_duty(idx, 0U, false);
    if (err == ESP_OK) {
        s_servo[idx].attached = false;
    }
    return err;
}

// 到位后自动松劲省电
// 舵机顶到尽头会叫
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

// 新动作先取消旧松劲
static void servo_cancel_release(int idx)
{
    if (s_servo[idx].release_timer != NULL) {
        esp_timer_stop(s_servo[idx].release_timer);
    }
}

// 排一个到点松劲
static void servo_schedule_release(int idx)
{
#if BSP_SERVO_RELEASE_MS > 0
    if (s_servo[idx].release_timer == NULL) {
        // 没建起来就一直使劲
        return;
    }
    esp_timer_start_once(s_servo[idx].release_timer, (uint64_t)BSP_SERVO_RELEASE_MS * 1000);
#else
    (void)idx;
#endif
}

// 等到点再往下走
static void servo_wait_until(int64_t due_us)
{
    while (1) {
        const int64_t remain_us = due_us - esp_timer_get_time();
        if (remain_us <= 0) {
            return;
        }

        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
        if (ticks == 0) {
            // 不满一刻就短等
            esp_rom_delay_us((uint32_t)remain_us);
            return;
        }

        // 让出处理器
        vTaskDelay(ticks);
    }
}

// 把硬件准备好
esp_err_t servo_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    // 三路共用这个定时器
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

    // 再开三个通道
    for (int i = 0; i < (int)SERVO_MAX; i++) {
        ledc_channel_config_t ccfg = {
            .gpio_num   = s_servo[i].gpio,
            .speed_mode = BSP_SERVO_MODE,
            .channel    = s_servo[i].channel,
            // 不用硬件渐变
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = BSP_SERVO_TIMER,
            // 先给零待会再设
            .duty       = 0,
            .hpoint     = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags      = { .output_invert = 0 },
        };

        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: ledc_channel_config(ch=%d gpio=%d) failed: %s",
                     s_servo_name[i], (int)s_servo[i].channel, (int)s_servo[i].gpio,
                     esp_err_to_name(err));
            // 建不起来就返回
            return err;
        }

        // 先归位再松劲
        s_servo[i].angle    = 0.0f;
        s_servo[i].target   = 0.0f;
        // 先允许写占空比
        s_servo[i].attached = true;

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

        // 建到点松劲的定时器
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

// 马上转到某个角度
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

    // 新动作撤掉旧松劲
    servo_cancel_release(idx);
    esp_err_t err = servo_apply(idx, a, true);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s -> %.1f deg (pulse %uus, duty %u)",
             s_servo_name[idx], a, (unsigned)servo_angle_to_pulse_us(a),
             (unsigned)servo_angle_to_duty(a));
    // 到点松劲防抖
    servo_schedule_release(idx);
    return ESP_OK;
}

// 慢慢转到某个角度
// 会卡住当前任务一会
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

    // 新动作撤掉旧松劲
    servo_cancel_release(idx);

    // 太久了会算错
    if (ms > 60000U) {
        ESP_LOGW(TAG, "%s smooth %ums too long, clamp to 60000ms", s_servo_name[(int)id], (unsigned)ms);
        ms = 60000U;
    }

    const float start = s_servo[idx].angle;
    const float diff  = target - start;

    // 目标先记下来
    s_servo[idx].target = target;

    if (fabsf(diff) < SERVO_ANGLE_EPS) {
        return servo_apply(idx, target, false);
    }

    // 每步一度别太碎
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

        // 只改现在角度
        esp_err_t err = servo_apply(idx, a, false);
        if (err != ESP_OK) {
            return err;
        }

        // 按时间点走不累积误差
        servo_wait_until(t0 + total_us * (int64_t)i / (int64_t)steps);
    }

    // 收尾停在目标角度
    esp_err_t err = servo_apply(idx, target, false);
    if (err == ESP_OK) {
        // 到点松劲
        servo_schedule_release(idx);
    }
    return err;
}

// 按百分比转过去
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

    // 零是全关百是全开
    return servo_set_angle(id, (float)percent * 180.0f / 100.0f);
}

// 查现在在哪角度
float servo_get_angle(servo_id_t id)
{
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    // 慢慢转时是中间值
    return s_servo[id].angle;
}

// 查要去的角度
float servo_get_target(servo_id_t id)
{
    if (!servo_id_valid(id)) {
        return 0.0f;
    }
    // 慢慢转时是终点
    return s_servo[id].target;
}

// 直接给脉宽调试
esp_err_t servo_set_pulse_us(servo_id_t id, uint32_t us)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int idx = (int)id;

    // 别给非法脉宽顶坏
    if (us < (uint32_t)BSP_SERVO_MIN_PULSE_US) {
        us = (uint32_t)BSP_SERVO_MIN_PULSE_US;
    }
    if (us > (uint32_t)BSP_SERVO_MAX_PULSE_US) {
        us = (uint32_t)BSP_SERVO_MAX_PULSE_US;
    }

    // 只改脉宽不动角度
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

// 松劲不再使劲
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

// 恢复使劲
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
        // 本来就使劲直接返回
        return ESP_OK;
    }

    // 按上次的占空比恢复
    esp_err_t err = servo_write_duty(idx, s_servo[idx].last_duty, false);
    if (err == ESP_OK) {
        s_servo[idx].attached = true;
        ESP_LOGI(TAG, "%s attached (restore duty %u)", s_servo_name[idx],
                 (unsigned)s_servo[idx].last_duty);
    }
    return err;
}

// 舵机号换名字
const char *servo_name(servo_id_t id)
{
    return servo_id_valid(id) ? s_servo_name[id] : "unknown";
}

// 名字反查舵机号
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
