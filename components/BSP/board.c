/**
 * @file  board.c
 * @brief 板级初始化 —— 按正确顺序把所有外设拉起来
 *
 * 【设计原则：配件没到也能跑】
 *   每个外设单独 try，失败只打警告、不中断流程、不返回致命错误。
 *   这样你可以先烧录、先连 WiFi、先用 MQTT 和按键把业务链路跑通，
 *   等传感器/舵机/语音模块到货了插上就能用，一行代码都不用改。
 *
 * 【初始化顺序为什么是这样】
 *   I2C / ADC 是共享总线，必须最先建好；
 *   灯带(RMT) 和 舵机/风扇(LEDC) 互不干扰，顺序随意；
 *   传感器依赖 I2C + ADC，所以排在它们之后；
 *   OLED 依赖 I2C，也是之后；
 *   按键和语音最后（它们会起任务，早点起也行，但放后面日志更整齐）。
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

static bool s_inited = false;

/* 记录各模块初始化结果，最后统一汇报，方便一眼看出"哪个配件还没插/没驱动起来" */
typedef struct {
    const char *name;
    esp_err_t   err;
} init_result_t;

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

    /* ---- 1. 共享总线 ---- */
    results[n].name = "i2c_bus";
    results[n].err  = i2c_bus_init();
    const esp_err_t i2c_err = results[n].err;
    n++;

    results[n].name = "adc_bus";
    results[n].err  = adc_bus_init();
    n++;

    /* I2C 通了才有意义扫描，扫一遍把挂在上面的芯片列出来 */
    if (i2c_err == ESP_OK) {
        int found = i2c_bus_scan();
        ESP_LOGI(TAG, "I2C scan done, %d device(s) found", found);
    } else {
        ESP_LOGW(TAG, "I2C bus init failed (0x%x), skip scan", i2c_err);
    }

    /* ---- 2. 执行器 ---- */
    results[n].name = "led strip x4";
    results[n].err  = led_init();
    n++;

    results[n].name = "servo x3";
    results[n].err  = servo_init();
    n++;

    results[n].name = "fan pwm";
    results[n].err  = fan_init();
    n++;

    /* ---- 3. 传感器（依赖 I2C + ADC） ---- */
    results[n].name = "sensors";
    results[n].err  = sensor_init();
    n++;

    /* ---- 4. 显示 ---- */
    results[n].name = "oled";
    results[n].err  = oled_init();
    n++;

    /* ---- 5. 人机输入 ---- */
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
    /* ---- 6. I²S 数字麦克风（INMP441）----
     * ★ 放在 voice 之后、且【不管语音来源开关】都初始化：
     *   · 它的作用是"让用户能随时串口敲 `mic` 确认麦克风工作没有"。
     *     麦克风到货后插上线就能测，不用先改 menuconfig 重新编译 ——
     *     这正是本任务"插上就能测"的要求。
     *   · 麦克风没接时 i2s_mic_init() 依然返回 ESP_OK（I²S 控制器和引脚是
     *     ESP32 自己的），真正"没插"会体现在 `mic` 命令读到的电平上。
     *     所以这一项在 reports 里正常是 [ OK ]，不代表麦克风一定接了。 */
    results[n].name = "i2s mic";
    results[n].err  = i2s_mic_init();
    n++;
#endif

    /* ---- 汇报 ---- */
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

    /* ★ 单独打一条"麦克风到底在不在拾音"的结论。
     *   上面那行 [ OK ] 只说明 I²S 控制器建起来了，和"麦克风有没有插"无关，
     *   容易被误读。这里真读一次看电平，把结论直接写在日志里。 */
#if BSP_I2S_MIC_ENABLE
    if (results[n - 1].err == ESP_OK) {
        int16_t probe[320] = { 0 };     /* 20ms @16KHz */
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

    /* 注意：返回 ESP_ERR_NOT_FOUND 并【不是】致命错误。
     * 调用方可以继续跑 —— 缺哪个配件就只是那个功能不可用。 */
    ESP_LOGW(TAG, "======== board init done, %d/%d module(s) unavailable ========",
             failed, n);
    return ESP_ERR_NOT_FOUND;
}

i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return i2c_bus_get_handle();
}

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
