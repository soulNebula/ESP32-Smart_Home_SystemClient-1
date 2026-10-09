// 风扇驱动

#include "fan.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "esp_assert.h"
#include "esp_log.h"

static const char *TAG = "FAN";

// 满量程按十位算
#define FAN_FULL_SCALE  1023U

// 分辨率改了要报错
ESP_STATIC_ASSERT(BSP_FAN_RES == LEDC_TIMER_10_BIT, "FAN_FULL_SCALE assumes 10-bit resolution");

static bool    s_inited = false;
// 记住当前风速
static uint8_t s_speed  = 0;

// 把硬件准备好
esp_err_t fan_init(void) {
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

    return ESP_OK;
}

// 按百分比调风速
esp_err_t fan_set_speed(uint8_t percent) {
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
uint8_t fan_get_speed(void) {
    return s_speed;
}

// 直接把风扇关掉
esp_err_t fan_off(void) {
    return fan_set_speed(0);
}

// 只认开和关
esp_err_t fan_set_power(bool on) {
    // 开就满速关就停
    return fan_set_speed(on ? 100 : 0);
}

// 查风扇转没转
bool fan_get_power(void) {
    return s_speed > 0;
}

