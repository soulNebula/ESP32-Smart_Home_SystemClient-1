/*
 * 模块：
 *   板级开机准备。上电后按顺序把整块板子的硬件拉起来，被 main.c 调用，
 *   自己向下调 i2c_bus / adc_bus / led / servo / fan / sensor /
 *   oled / key / adkey / voice / i2s_mic 这些模块。
 *
 * 功能：
 *   按顺序把硬件准备好
 *   缺配件也照常开机
 *   打印引脚对照表
 */
#include "board.h"

#include "esp_log.h"
#include "esp_err.h"

#include "i2c_bus.h"
#include "adc_bus.h"
#include "led.h"
#include "key.h"
#include "adkey.h"
#include "servo.h"
#include "fan.h"
#include "sensor.h"
#include "oled.h"
#include "voice.h"
#include "i2s_mic.h"

static const char *TAG = "BOARD";

static bool s_inited = false;   /* 功能：记住已开机 */

/* 功能：记下各件初始化结果 */
typedef struct {
    const char *name;
    esp_err_t   err;
} init_result_t;

/* 功能：按顺序把硬件准备好 */
esp_err_t board_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "board already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "================ board init begin ================");
    board_print_pinmap();

    init_result_t results[14];
    int n = 0;

    /* 功能：先建共享总线 */
    results[n].name = "i2c_bus";
    results[n].err  = i2c_bus_init();
    const esp_err_t i2c_err = results[n].err;
    n++;

    results[n].name = "adc_bus";
    results[n].err  = adc_bus_init();
    n++;

    /* 功能：列一下挂着谁 */
    if (i2c_err == ESP_OK) {
        int found = i2c_bus_scan();
        ESP_LOGI(TAG, "I2C scan done, %d device(s) found", found);
    } else {
        ESP_LOGW(TAG, "I2C bus init failed (0x%x), skip scan", i2c_err);
    }

    /* 功能：起灯舵机风扇 */
    results[n].name = "led strip x4";
    results[n].err  = led_init();
    n++;

    results[n].name = "servo x3";
    results[n].err  = servo_init();
    n++;

    results[n].name = "fan pwm";
    results[n].err  = fan_init();
    n++;

    /* 功能：起温湿度光照雨滴 */
    results[n].name = "sensors";
    results[n].err  = sensor_init();
    n++;

    /* 功能：起屏幕 */
    results[n].name = "oled";
    results[n].err  = oled_init();
    n++;

    /* 功能：起按键和语音 */
    results[n].name = "key x2";
    results[n].err  = key_init();
    n++;

    results[n].name = "adkey pad";
    results[n].err  = adkey_init();
    n++;

    results[n].name = "voice uart";
    results[n].err  = voice_init();
    n++;

#if BSP_I2S_MIC_ENABLE
    /* 功能：起麦克风，没插也能测 */
    results[n].name = "i2s mic";
    results[n].err  = i2s_mic_init();
    n++;
#endif

    /* 功能：报告各件结果 */
    int failed = 0;
    for (int i = 0; i < n; i++) {
        if (results[i].err == ESP_OK) {
            ESP_LOGI(TAG, "  [ OK ] %-14s", results[i].name);
        } else {
            ESP_LOGW(TAG, "  [FAIL] %-14s  err=0x%x (%s)",
                     results[i].name, results[i].err, esp_err_to_name(results[i].err));
            failed++;
        }
    }

    /* 功能：实读一次看拾音 */
#if BSP_I2S_MIC_ENABLE
    if (results[n - 1].err == ESP_OK) {
        int16_t probe[320] = { 0 };     /* 功能：读一小段音频 */
        size_t  got = 0;
        (void)i2s_mic_read(probe, sizeof(probe) / sizeof(probe[0]), &got, 300);
        i2s_mic_level_t lv;
        i2s_mic_read_level(&lv);
        if (i2s_mic_is_ready()) {
            ESP_LOGI(TAG, "  [MIC ] INMP441 正在拾音：rms=%d peak=%d 峰峰值=%d dBFS=%d.%d",
                     lv.rms, lv.peak, lv.peak_to_peak,
                     lv.db_x10 / 10, (lv.db_x10 < 0 ? -lv.db_x10 : lv.db_x10) % 10);
        } else {
            ESP_LOGW(TAG, "  [MIC ] 没读到有效音频（rms=%d peak=%d）—— INMP441 可能没接。"
                          "它是 I²S【不是 I²C】：SCK=GPIO%d WS=GPIO%d SD=GPIO%d，"
                          "L/R 必须接 GND，VDD 接 3V3。串口敲 `mic` 看实时电平。",
                     lv.rms, lv.peak,
                     (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
        }
    }
#endif

    s_inited = true;

    if (failed == 0) {
        ESP_LOGI(TAG, "================ board init OK ================");
        return ESP_OK;
    }

    /* 功能：缺件不算致命 */
    ESP_LOGW(TAG, "======== board init done, %d/%d module(s) unavailable ========",
             failed, n);
    return ESP_ERR_NOT_FOUND;
}

/* 功能：交出总线把手 */
i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return i2c_bus_get_handle();
}

/* 功能：打印引脚对照表 */
void board_print_pinmap(void)
{
    ESP_LOGI(TAG, "----------------- PIN MAP (source: board_config.h) -----------------");
    ESP_LOGI(TAG, "  I2C    SDA=GPIO%-2d SCL=GPIO%-2d          -> OLED/SHT30/AHT20/BH1750",
             (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    ESP_LOGI(TAG, "  ADC    LIGHT=GPIO%-2d RAIN=GPIO%-2d       -> 光敏电阻 / 雨滴AO",
             (int)BSP_GPIO_LIGHT_ADC, (int)BSP_GPIO_RAIN_AO);
    ESP_LOGI(TAG, "  WS2812 LIVING=%-2d KITCHEN=%-2d BEDROOM=%-2d BATH=%-2d",
             (int)BSP_WS2812_GPIO_LIVING, (int)BSP_WS2812_GPIO_KITCHEN,
             (int)BSP_WS2812_GPIO_BEDROOM, (int)BSP_WS2812_GPIO_BATH);
    ESP_LOGI(TAG, "  SERVO  CURTAIN=%-2d WINDOW=%-2d DOOR=%-2d",
             (int)BSP_SERVO_GPIO_CURTAIN, (int)BSP_SERVO_GPIO_WINDOW,
             (int)BSP_SERVO_GPIO_DOOR);
    ESP_LOGI(TAG, "  FAN    PWM=%-2d TACH=%-2d",
             (int)BSP_FAN_GPIO_PWM, (int)BSP_FAN_GPIO_TACH);
    ESP_LOGI(TAG, "  KEY    KEY1=%-2d KEY2=%-2d",
             (int)BSP_KEY_GPIO_KEY1, (int)BSP_KEY_GPIO_KEY2);
    ESP_LOGI(TAG, "  VOICE  TX=%-2d RX=%-2d  (ESP TX -> module RXD)",
             (int)BSP_VOICE_TX_GPIO, (int)BSP_VOICE_RX_GPIO);
    ESP_LOGI(TAG, "  I2S MIC SCK=%-2d WS=%-2d SD=%-2d  (INMP441, I²S 不是 I²C! L/R->GND, VDD->3V3)",
             (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
    ESP_LOGI(TAG, "  STATUS LED = GPIO%d (onboard WS2812)", (int)BSP_STATUS_LED_GPIO);
    ESP_LOGI(TAG, "-------------------------------------------------------------------");
}
