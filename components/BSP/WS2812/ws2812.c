#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

// 直接写寄存器快
#include "hal/gpio_ll.h"
// 拿到GPIO寄存器
#include "soc/gpio_struct.h"
// 芯片通道内存数
#include "soc/soc_caps.h"

#include "ws2812.h"
#include "board_config.h"

static const char *TAG = "WS2812";

// 灯带时序，单位纳秒
#define WS2812_T0H_NS           300
#define WS2812_T0L_NS           900
#define WS2812_T1H_NS           900
#define WS2812_T1L_NS           300
// 复位要超50微秒
#define WS2812_RESET_NS         60000

// 复位延时按微秒算
#define WS2812_BITBANG_RESET_US 60

// S3 只有四路
#define WS2812_MAX_STRIPS       4

// 发送队列深度
#define WS2812_TRANS_QUEUE_DEPTH    4

// 等发完的余量
#define WS2812_TX_TIMEOUT_MARGIN_MS 100
// 每颗灯约30微秒
#define WS2812_US_PER_LED           30

// 算不出主频就兜底
// S3 跑240兆
#define WS2812_FALLBACK_TICKS_PER_US    240

// 灯带长这样
struct ws2812_strip_s {
    // 数据脚
    gpio_num_t              gpio;
    // 灯珠数量
    uint32_t                led_num;
    // 颜色缓存
    uint8_t                *buf;
    // 发送通道
    rmt_channel_handle_t    chan;
    // 自己写的编码器
    rmt_encoder_handle_t    encoder;
};

// 自己写的编码器
typedef struct {
    rmt_encoder_t           base;
    // 字节变波形
    rmt_encoder_handle_t    bytes_encoder;
    // 补一段复位码
    rmt_encoder_handle_t    copy_encoder;
    // 0发数据1复位
    int                     state;
    // 复位用的码
    rmt_symbol_word_t       reset_code;
} ws2812_encoder_t;

// 数已建了几条
static portMUX_TYPE s_strip_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_strip_cnt = 0;

// 纳秒换成计数
static uint32_t ws2812_ns_to_ticks(uint32_t ns, uint32_t res_hz)
{
    uint64_t ticks = ((uint64_t)ns * (uint64_t)res_hz) / 1000000000ULL;

    if (ticks == 0) {
        // 0不合法给1
        ticks = 1;
    }
    if (ticks > 0x7FFFULL) {
        // 最多15位
        ticks = 0x7FFFULL;
    }
    return (uint32_t)ticks;
}

// 夹到只占一块内存
static size_t ws2812_mem_block_symbols(void)
{
    size_t block = (size_t)BSP_WS2812_MEM_BLOCK;
    const size_t one_block = (size_t)SOC_RMT_MEM_WORDS_PER_CHANNEL;

    if (block < one_block) {
        // 小了非法要补齐
        block = one_block;
    }
    if ((block & 0x1U) != 0U) {
        // 必须凑成偶数
        block++;
    }
    if (block > one_block) {
        // 占了就少一路
        block = one_block;
    }
    return block;
}

// 算等多久算超时
static int ws2812_tx_timeout_ms(uint32_t led_num)
{
    return (int)((led_num * WS2812_US_PER_LED) / 1000U) + WS2812_TX_TIMEOUT_MARGIN_MS;
}

// 把颜色编成波形
static size_t ws2812_encoder_encode(rmt_encoder_t *encoder, rmt_channel_handle_t tx_channel,
                                    const void *primary_data, size_t data_size,
                                    rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = ws_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = ws_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws_encoder->state) {
    // 先发颜色
    case 0:
        encoded_symbols += bytes_encoder->encode(bytes_encoder, tx_channel,
                                                 primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            // 发完换复位码
            ws_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            // 满了先让去发
            goto out;
        }
        // 故意往下走
    // 再发复位码
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, tx_channel,
                                                &ws_encoder->reset_code,
                                                sizeof(ws_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            // 发完回到开头
            ws_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        break;
    default:
        break;
    }

out:
    *ret_state = state;
    return encoded_symbols;
}

// 把占的东西还回去
static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);

    if (ws_encoder->bytes_encoder != NULL) {
        rmt_del_encoder(ws_encoder->bytes_encoder);
    }
    if (ws_encoder->copy_encoder != NULL) {
        rmt_del_encoder(ws_encoder->copy_encoder);
    }
    free(ws_encoder);
    return ESP_OK;
}

// 清干净重来
static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_encoder = __containerof(encoder, ws2812_encoder_t, base);

    if (ws_encoder->bytes_encoder != NULL) {
        rmt_encoder_reset(ws_encoder->bytes_encoder);
    }
    if (ws_encoder->copy_encoder != NULL) {
        rmt_encoder_reset(ws_encoder->copy_encoder);
    }
    ws_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

// 建一个编码器
static esp_err_t ws2812_new_encoder(uint32_t res_hz, rmt_encoder_handle_t *ret_encoder)
{
    if (ret_encoder == NULL || res_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_encoder = NULL;

    ws2812_encoder_t *ws_encoder = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    if (ws_encoder == NULL) {
        ESP_LOGE(TAG, "no mem for ws2812 encoder");
        return ESP_ERR_NO_MEM;
    }

    ws_encoder->base.encode = ws2812_encoder_encode;
    ws_encoder->base.reset  = ws2812_encoder_reset;
    ws_encoder->base.del    = ws2812_encoder_del;
    ws_encoder->state       = RMT_ENCODING_RESET;

    // 0和1各自的波形
    const rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = ws2812_ns_to_ticks(WS2812_T0H_NS, res_hz),
            .level1 = 0,
            .duration1 = ws2812_ns_to_ticks(WS2812_T0L_NS, res_hz),
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = ws2812_ns_to_ticks(WS2812_T1H_NS, res_hz),
            .level1 = 0,
            .duration1 = ws2812_ns_to_ticks(WS2812_T1L_NS, res_hz),
        },
        .flags.msb_first = 1,
    };

    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &ws_encoder->bytes_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        free(ws_encoder);
        return err;
    }

    const rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &ws_encoder->copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        rmt_del_encoder(ws_encoder->bytes_encoder);
        free(ws_encoder);
        return err;
    }

    // 两段低电平凑复位
    uint32_t reset_half_ticks = ws2812_ns_to_ticks(WS2812_RESET_NS, res_hz) / 2U;
    if (reset_half_ticks == 0) {
        reset_half_ticks = 1;
    }
    ws_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_half_ticks,
        .level1 = 0,
        .duration1 = reset_half_ticks,
    };

    *ret_encoder = &ws_encoder->base;
    return ESP_OK;
}

// 纳秒换CPU周期
static inline IRAM_ATTR uint32_t ws2812_ns_to_cycles(uint32_t ns)
{
    uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();

    if (ticks_per_us == 0) {
        ticks_per_us = WS2812_FALLBACK_TICKS_PER_US;
    }
    return (uint32_t)(((uint64_t)ns * (uint64_t)ticks_per_us) / 1000ULL);
}

// 翻转引脚发数据
static void IRAM_ATTR ws2812_bitbang_send(gpio_num_t gpio, const uint8_t *grb, uint32_t len)
{
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

// 建一条灯带
esp_err_t ws2812_new_strip(gpio_num_t gpio, uint32_t led_num, ws2812_strip_handle_t *out)
{
    esp_err_t err = ESP_FAIL;
    ws2812_strip_handle_t h = NULL;
    bool chan_enabled = false;
    bool slot_taken = false;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (led_num == 0) {
        ESP_LOGE(TAG, "led_num must be > 0");
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)gpio < 0) {
        ESP_LOGE(TAG, "invalid data gpio: %d", (int)gpio);
        return ESP_ERR_INVALID_ARG;
    }

    // 先占一个通道
    portENTER_CRITICAL(&s_strip_lock);
    if (s_strip_cnt >= WS2812_MAX_STRIPS) {
        portEXIT_CRITICAL(&s_strip_lock);
        ESP_LOGE(TAG, "no free RMT TX channel (max %d strips on ESP32-S3)",
                 WS2812_MAX_STRIPS);
        return ESP_ERR_NO_MEM;
    }
    s_strip_cnt++;
    slot_taken = true;
    portEXIT_CRITICAL(&s_strip_lock);

    h = calloc(1, sizeof(struct ws2812_strip_s));
    if (h == NULL) {
        ESP_LOGE(TAG, "no mem for strip handle");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    h->buf = calloc((size_t)led_num * 3U, sizeof(uint8_t));
    if (h->buf == NULL) {
        ESP_LOGE(TAG, "no mem for %u LED buffer", (unsigned)led_num);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    h->gpio = gpio;
    h->led_num = led_num;

    const size_t mem_block = ws2812_mem_block_symbols();
    if (mem_block != (size_t)BSP_WS2812_MEM_BLOCK) {
        ESP_LOGW(TAG, "BSP_WS2812_MEM_BLOCK=%d clamped to %d symbols "
                      "(1 memory block/channel, keeps all 4 RMT TX channels usable)",
                 (int)BSP_WS2812_MEM_BLOCK, (int)mem_block);
    }

    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = BSP_WS2812_RMT_RES_HZ,
        .mem_block_symbols = mem_block,
        .trans_queue_depth = WS2812_TRANS_QUEUE_DEPTH,
        .intr_priority = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 1)
        // 空闲时输出低
        .flags.init_level = 0,
#endif
    };

    err = rmt_new_tx_channel(&tx_cfg, &h->chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel(GPIO%d) failed: %s",
                 (int)gpio, esp_err_to_name(err));
        h->chan = NULL;
        goto fail;
    }

    err = ws2812_new_encoder(BSP_WS2812_RMT_RES_HZ, &h->encoder);
    if (err != ESP_OK) {
        goto fail;
    }

    err = rmt_enable(h->chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        goto fail;
    }
    chan_enabled = true;

    ESP_LOGI(TAG, "strip on GPIO%d: %u LED(s), res %dHz, mem_block %d symbols",
             (int)gpio, (unsigned)led_num, (int)BSP_WS2812_RMT_RES_HZ, (int)mem_block);

    *out = h;
    return ESP_OK;

fail:
    if (h != NULL) {
        if (h->encoder != NULL) {
            rmt_del_encoder(h->encoder);
        }
        if (h->chan != NULL) {
            if (chan_enabled) {
                rmt_disable(h->chan);
            }
            rmt_del_channel(h->chan);
        }
        if (h->buf != NULL) {
            free(h->buf);
        }
        free(h);
    }
    if (slot_taken) {
        portENTER_CRITICAL(&s_strip_lock);
        s_strip_cnt--;
        portEXIT_CRITICAL(&s_strip_lock);
    }
    return err;
}

// 给一颗灯上色
esp_err_t ws2812_set_pixel(ws2812_strip_handle_t h, uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (h == NULL || h->buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index >= h->led_num) {
        ESP_LOGE(TAG, "set_pixel index %u out of range (led_num=%u)",
                 (unsigned)index, (unsigned)h->led_num);
        return ESP_ERR_INVALID_ARG;
    }

    // 顺序是绿红蓝
    uint8_t *p = &h->buf[(size_t)index * 3U];
    p[0] = g;
    p[1] = r;
    p[2] = b;
    return ESP_OK;
}

// 整条一个颜色
esp_err_t ws2812_set_all(ws2812_strip_handle_t h, uint8_t r, uint8_t g, uint8_t b)
{
    if (h == NULL || h->buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0; i < h->led_num; i++) {
        uint8_t *p = &h->buf[(size_t)i * 3U];
        p[0] = g;
        p[1] = r;
        p[2] = b;
    }
    return ESP_OK;
}

// 把颜色发出去
esp_err_t ws2812_refresh(ws2812_strip_handle_t h)
{
    if (h == NULL || h->buf == NULL || h->chan == NULL || h->encoder == NULL) {
        ESP_LOGE(TAG, "refresh with invalid strip handle");
        return ESP_ERR_INVALID_ARG;
    }

    const rmt_transmit_config_t tx_cfg = {
        // 只发一次
        .loop_count = 0,
        // 发完拉低锁色
        .flags.eot_level = 0,
    };

    esp_err_t err = rmt_transmit(h->chan, h->encoder, h->buf,
                                 (size_t)h->led_num * 3U, &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_tx_wait_all_done(h->chan, ws2812_tx_timeout_ms(h->led_num));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

// 清成全黑
esp_err_t ws2812_clear(ws2812_strip_handle_t h, bool do_refresh)
{
    if (h == NULL || h->buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(h->buf, 0, (size_t)h->led_num * 3U);

    if (do_refresh) {
        return ws2812_refresh(h);
    }
    return ESP_OK;
}

// 报灯珠数量
uint32_t ws2812_get_num(ws2812_strip_handle_t h)
{
    if (h == NULL) {
        return 0;
    }
    return h->led_num;
}

// 翻转引脚直接发
esp_err_t ws2812_bitbang_write(gpio_num_t gpio, const uint8_t *grb, uint32_t len)
{
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
