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

// 满量程按十位算
#define FAN_FULL_SCALE  1023U

// 分辨率改了要报错
ESP_STATIC_ASSERT(BSP_FAN_RES == LEDC_TIMER_10_BIT, "FAN_FULL_SCALE assumes 10-bit resolution");

static bool    s_inited = false;
// 记住当前风速
static uint8_t s_speed  = 0;

// 读转速是选配
#if BSP_FAN_TACH_ENABLE

// 数一秒的脉冲
#define FAN_TACH_WINDOW_US      1000000U
// 每百微秒看一次
#define FAN_TACH_POLL_US        100U
// 每转两个脉冲
#define FAN_TACH_PULSES_PER_REV 2U

static bool s_tach_inited = false;

// 把测速脚准备好
static esp_err_t fan_tach_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (int)BSP_FAN_GPIO_TACH,
        .mode         = GPIO_MODE_INPUT,
        // 测速脚要上拉
        .pull_up_en   = GPIO_PULLUP_ENABLE,
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

// 测速开关收尾
#endif

// 把硬件准备好
esp_err_t fan_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    // 定时器定成高频率
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

    // 再开出波形的通道
    ledc_channel_config_t ccfg = {
        .gpio_num   = BSP_FAN_GPIO_PWM,
        .speed_mode = BSP_FAN_MODE,
        .channel    = BSP_FAN_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BSP_FAN_TIMER,
        // 开机先停转
        .duty       = 0,
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
    // 测速坏了也能调速
    (void)fan_tach_init();
#endif

    return ESP_OK;
}

// 按百分比调风速
esp_err_t fan_set_speed(uint8_t percent)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > 100) {
        percent = 100;
    }

    // 零就彻底停住
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

// 查现在多大风
uint8_t fan_get_speed(void)
{
    return s_speed;
}

// 直接把风扇关掉
esp_err_t fan_off(void)
{
    return fan_set_speed(0);
}

// 只认开和关
esp_err_t fan_set_power(bool on)
{
    // 开就满速关就停
    return fan_set_speed(on ? 100 : 0);
}

// 查风扇转没转
bool fan_get_power(void)
{
    return s_speed > 0;
}

#if BSP_FAN_TACH_ENABLE

// 数脉冲算转速
// 会卡住大约一秒
uint32_t fan_get_rpm(void)
{
    if (!s_inited || !s_tach_inited) {
        return 0;
    }

    uint32_t pulses = 0;
    int      last   = gpio_get_level(BSP_FAN_GPIO_TACH);
    const int64_t t0 = esp_timer_get_time();

    while ((esp_timer_get_time() - t0) < (int64_t)FAN_TACH_WINDOW_US) {
        // 采一批歇一小会
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

    // 脉冲换算成转速
    return (uint32_t)(((uint64_t)pulses * 60ULL) / (uint64_t)FAN_TACH_PULSES_PER_REV);
}

// 没开测速
#else

uint32_t fan_get_rpm(void)
{
    // 没开测速就返回零
    return 0;
}

// 测速开关收尾
#endif
