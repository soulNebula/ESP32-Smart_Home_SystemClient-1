/**
 * @file  fan.c
 * @brief 风扇调速实现（MOS 管 PWM，GPIO18，LEDC 25kHz / 10bit）
 *
 * 【实现要点】
 *   1) 25kHz 超出人耳听觉范围，风扇不会"吱吱"叫；10bit → duty 0~1023。
 *   2) 0% 时 duty 真给 0（不是给 1/1023），保证完全停转。
 *   3) 测速 TACH 默认关闭（BSP_FAN_TACH_ENABLE = 0），此时 fan_get_rpm() 直接返回 0；
 *      打开后本文件用「GPIO 边沿计数窗口法」测 1 秒脉冲数，按 2 脉冲/转换算 RPM。
 *
 * 所有引脚/参数都取自 board_config.h，本文件不硬编码任何 GPIO 号。
 */
#include "fan.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_assert.h"
#include "esp_log.h"

#if BSP_FAN_TACH_ENABLE
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "FAN";

#define FAN_FULL_SCALE  1023U   /* 2^10 - 1，对应 BSP_FAN_RES = LEDC_TIMER_10_BIT */

/* 满量程是按 10bit 写死的，若 board_config.h 改了分辨率这里会立刻编译报错，
 * 提醒同步修改 FAN_FULL_SCALE，避免占空比算错 */
ESP_STATIC_ASSERT(BSP_FAN_RES == LEDC_TIMER_10_BIT, "FAN_FULL_SCALE assumes 10-bit resolution");

static bool    s_inited = false;
static uint8_t s_speed  = 0;    /* 0~100，软件记录的实际转速 */

/* ------------------------------------------------------------------ */
/*  测速（可选，默认关闭）                                             */
/* ------------------------------------------------------------------ */
#if BSP_FAN_TACH_ENABLE

#define FAN_TACH_WINDOW_US      1000000U  /* 采样窗口 1 秒 */
#define FAN_TACH_POLL_US        100U      /* 采样间隔 100us：1 秒窗口内 1 个脉冲也数得到（≈30RPM 分辨率） */
#define FAN_TACH_PULSES_PER_REV 2U        /* 4 线风扇标准：每转 2 个脉冲 */

static bool s_tach_inited = false;

static esp_err_t fan_tach_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (int)BSP_FAN_GPIO_TACH,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 风扇 TACH 是开漏输出，需要上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tach gpio%d config failed: %s", (int)BSP_FAN_GPIO_TACH, esp_err_to_name(err));
        return err;
    }

    s_tach_inited = true;
    ESP_LOGI(TAG, "tach enabled on gpio%d (1s window, %u pulses/rev)",
             (int)BSP_FAN_GPIO_TACH, (unsigned)FAN_TACH_PULSES_PER_REV);
    return ESP_OK;
}

#endif /* BSP_FAN_TACH_ENABLE */

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t fan_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    /* ---- 定时器：25kHz / 10bit ---- */
    ledc_timer_config_t tcfg = {
        .speed_mode      = BSP_FAN_MODE,
        .duty_resolution = BSP_FAN_RES,
        .timer_num       = BSP_FAN_TIMER,
        .freq_hz         = BSP_FAN_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- 通道 ---- */
    ledc_channel_config_t ccfg = {
        .gpio_num   = BSP_FAN_GPIO_PWM,
        .speed_mode = BSP_FAN_MODE,
        .channel    = BSP_FAN_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BSP_FAN_TIMER,
        .duty       = 0,          /* 初始停转 */
        .hpoint     = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags      = { .output_invert = 0 },
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config(ch=%d gpio=%d) failed: %s",
                 (int)BSP_FAN_CHANNEL, (int)BSP_FAN_GPIO_PWM, esp_err_to_name(err));
        return err;
    }

    s_speed  = 0;
    s_inited = true;

    ESP_LOGI(TAG, "init ok: %dHz / %d bit, pwm=gpio%d, stopped",
             BSP_FAN_FREQ_HZ, (int)BSP_FAN_RES, (int)BSP_FAN_GPIO_PWM);

#if BSP_FAN_TACH_ENABLE
    /* 测速失败不影响调速：只是读不到 RPM 而已 */
    (void)fan_tach_init();
#endif

    return ESP_OK;
}

esp_err_t fan_set_speed(uint8_t percent)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > 100) {
        percent = 100;
    }

    /* 0 必须真给 0：完全停转（不是最小占空比） */
    const uint32_t duty = ((uint32_t)percent * FAN_FULL_SCALE) / 100U;

    esp_err_t err = ledc_set_duty(BSP_FAN_MODE, BSP_FAN_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty(duty=%u) failed: %s", (unsigned)duty, esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(BSP_FAN_MODE, BSP_FAN_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_update_duty failed: %s", esp_err_to_name(err));
        return err;
    }

    s_speed = percent;
    ESP_LOGI(TAG, "speed %u%% (duty %u/%u)", (unsigned)percent, (unsigned)duty, (unsigned)FAN_FULL_SCALE);
    return ESP_OK;
}

uint8_t fan_get_speed(void)
{
    return s_speed;
}

esp_err_t fan_off(void)
{
    return fan_set_speed(0);
}

esp_err_t fan_set_power(bool on)
{
    /* 兼容旧接口：true = 全速，false = 停 */
    return fan_set_speed(on ? 100 : 0);
}

bool fan_get_power(void)
{
    return s_speed > 0;
}

#if BSP_FAN_TACH_ENABLE

/**
 * @brief 测转速：1 秒窗口内数上升沿（TACH 开漏输出，需要上拉）
 * @note  会阻塞约 1 秒。每毫秒做 10 次 100us 采样，然后 vTaskDelay(1) 让出 CPU，
 *        不会长时间霸占（1ms 的空隙远小于最低转速下的脉冲周期，不会漏计数）。
 *        （更好的方案：用 PCNT 硬件计数，完全不占 CPU；driver 组件已含 pcnt。）
 */
uint32_t fan_get_rpm(void)
{
    if (!s_inited || !s_tach_inited) {
        return 0;
    }

    uint32_t pulses = 0;
    int      last   = gpio_get_level(BSP_FAN_GPIO_TACH);
    const int64_t t0 = esp_timer_get_time();

    while ((esp_timer_get_time() - t0) < (int64_t)FAN_TACH_WINDOW_US) {
        /* 1ms 采样一批（10 x 100us），采完让出 CPU 1ms */
        for (int k = 0; k < (1000 / FAN_TACH_POLL_US); k++) {
            const int level = gpio_get_level(BSP_FAN_GPIO_TACH);
            if (level != 0 && last == 0) {
                pulses++;
            }
            last = level;
            esp_rom_delay_us(FAN_TACH_POLL_US);
        }
        vTaskDelay(1);
    }

    /* 1 秒窗口：RPM = 脉冲数 * 60 / 每转脉冲数 */
    return (uint32_t)(((uint64_t)pulses * 60ULL) / (uint64_t)FAN_TACH_PULSES_PER_REV);
}

#else  /* !BSP_FAN_TACH_ENABLE */

uint32_t fan_get_rpm(void)
{
    /* 测速默认关闭（board_config.h 里 BSP_FAN_TACH_ENABLE = 0），不接测速线也能正常工作。
     *
     * 想启用 RPM 读数：
     *   1) 把 board_config.h 的 BSP_FAN_TACH_ENABLE 改成 1；
     *   2) 风扇 4 线中的第 3 脚（黄线 TACH）接到 BSP_FAN_GPIO_TACH（GPIO14）；
     *   3) 确认独立 5V 电源与开发板共地，否则测不到脉冲。
     * 启用后本文件用 GPIO 边沿计数窗口法测 1 秒脉冲数，按 2 脉冲/转换算。
     * （更好的方案：用 PCNT 硬件计数，完全不占 CPU；driver 组件已含 pcnt。） */
    return 0;
}

#endif /* BSP_FAN_TACH_ENABLE */
