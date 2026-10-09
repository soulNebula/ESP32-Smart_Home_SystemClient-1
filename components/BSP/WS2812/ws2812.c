// WS2812驱动
// 现在只提供板载状态灯用的 bitbang 驱动；RMT 灯带驱动已移除

#include <stdint.h>

// 开关中断用
#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_log.h"
// 标记放内存跑
#include "esp_attr.h"
// 读CPU周期数
#include "esp_cpu.h"
// 延时和主频
#include "esp_rom_sys.h"

#include "driver/gpio.h"

// 直接写寄存器快
#include "hal/gpio_ll.h"
// 拿到GPIO寄存器
#include "soc/gpio_struct.h"

#include "ws2812.h"

static const char *TAG = "WS2812";

// 灯带时序，单位纳秒
#define WS2812_T0H_NS           300
#define WS2812_T0L_NS           900
#define WS2812_T1H_NS           900
#define WS2812_T1L_NS           300

// 复位延时按微秒算
#define WS2812_BITBANG_RESET_US 60

// 算不出主频就兜底
// S3 跑240兆
#define WS2812_FALLBACK_TICKS_PER_US    240

// 纳秒换CPU周期
static inline IRAM_ATTR uint32_t ws2812_ns_to_cycles(uint32_t ns) {
    uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();

    if (ticks_per_us == 0) {
        ticks_per_us = WS2812_FALLBACK_TICKS_PER_US;
    }
    return (uint32_t)(((uint64_t)ns * (uint64_t)ticks_per_us) / 1000ULL);
}

// 翻转引脚发数据
static void IRAM_ATTR ws2812_bitbang_send(gpio_num_t gpio, const uint8_t *grb, uint32_t len) {
    const uint32_t t0h = ws2812_ns_to_cycles(WS2812_T0H_NS);
    const uint32_t t0l = ws2812_ns_to_cycles(WS2812_T0L_NS);
    const uint32_t t1h = ws2812_ns_to_cycles(WS2812_T1H_NS);
    const uint32_t t1l = ws2812_ns_to_cycles(WS2812_T1L_NS);
    // 一位的总时长
    const uint32_t t0_total = t0h + t0l;
    const uint32_t t1_total = t1h + t1l;
    const uint32_t gpio_num = (uint32_t)gpio;
    const uint32_t bytes = len * 3U;

    portDISABLE_INTERRUPTS();

    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t byte = grb[i];

        // 从高位开始发
        for (int bit = 7; bit >= 0; bit--) {
            // 都从同一刻起算
            uint32_t start = esp_cpu_get_cycle_count();

            if (byte & (uint8_t)(1U << bit)) {
                gpio_ll_set_level(&GPIO, gpio_num, 1);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t1h) {
                    // 空转等够时间
                }
                gpio_ll_set_level(&GPIO, gpio_num, 0);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t1_total) {
                    // 空转等够时间
                }
            } else {
                gpio_ll_set_level(&GPIO, gpio_num, 1);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t0h) {
                    // 空转等够时间
                }
                gpio_ll_set_level(&GPIO, gpio_num, 0);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t0_total) {
                    // 空转等够时间
                }
            }
        }
    }

    // 发完拉低再放行
    gpio_ll_set_level(&GPIO, gpio_num, 0);
    portENABLE_INTERRUPTS();

    // 拉低一阵复位
    esp_rom_delay_us(WS2812_BITBANG_RESET_US);
}

// 翻转引脚直接发
esp_err_t ws2812_bitbang_write(gpio_num_t gpio, const uint8_t *grb, uint32_t len) {
    if (grb == NULL) {
        ESP_LOGE(TAG, "bitbang: grb is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)gpio < 0) {
        ESP_LOGE(TAG, "bitbang: invalid data gpio: %d", (int)gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    // 只配一次脚免刷屏
    static int s_bitbang_cfg_gpio = -1;
    esp_err_t err;

    if ((int)gpio != s_bitbang_cfg_gpio) {
        // 先配脚再关中断
        err = gpio_reset_pin(gpio);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_reset_pin(%d) failed: %s", (int)gpio, esp_err_to_name(err));
            return err;
        }

        err = gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_set_direction(%d) failed: %s", (int)gpio, esp_err_to_name(err));
            return err;
        }

        s_bitbang_cfg_gpio = (int)gpio;
    }

    err = gpio_set_level(gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level(%d) failed: %s", (int)gpio, esp_err_to_name(err));
        return err;
    }

    ws2812_bitbang_send(gpio, grb, len);
    return ESP_OK;
}
