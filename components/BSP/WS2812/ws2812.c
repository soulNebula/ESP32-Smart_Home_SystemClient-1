/**
 * @file  ws2812.c
 * @brief WS2812/WS2812B 驱动实现（RMT 后端 + 位带后端）
 *
 * ===========================================================================
 *  两套后端
 * ===========================================================================
 *  1) RMT 后端（ws2812_new_strip / set_pixel / set_all / refresh / clear）
 *     用于 4 个房间的灯带。ESP32-S3 只有 4 个 RMT TX 通道，正好 4 路。
 *     - rmt_new_tx_channel() 建 TX 通道，resolution = BSP_WS2812_RMT_RES_HZ
 *     - 自定义 ws2812_encoder_t（照官方示例
 *       examples/peripherals/rmt/led_strip/main/led_strip_encoder.c 的思路）：
 *       内嵌 bytes_encoder（每 bit → 一个 RMT symbol）+ copy_encoder（复位码）
 *     - rmt_transmit() + rmt_tx_wait_all_done() 发送
 *  2) 位带后端（ws2812_bitbang_write）
 *     用于板载 RGB 状态灯（GPIO48）。S3 的 4 个 RMT 通道已经全给房间灯带了，
 *     状态灯刷新频率极低，关中断 ~30us/颗完全可接受。
 *
 * ===========================================================================
 *  ⚠ 位带后端的延时实现说明（和任务书里的写法有出入，理由如下）
 * ===========================================================================
 *  任务书建议"用 esp_rom_delay_us() 产生 0.3us / 0.9us"。
 *  但 esp_rom_delay_us(uint32_t us) 的参数是【整数微秒】，根本表达不了
 *  0.3us / 0.9us —— 传 0 只会得到函数调用开销（几十个周期，约 0.1us，太短，
 *  WS2812B 要求 T0H ≥ 0.25us），传 1 又变成 1us（太长，位周期会跑到 1.3us+）。
 *  所以这里：
 *    · 位的 4 段电平（T0H/T0L/T1H/T1L）用 CPU 周期计数（CCOUNT）做精确延时，
 *      每个 bit 的周期从一个统一的时间原点量起，误差只有几条指令；
 *    · esp_rom_delay_us() 用在复位段（>50us），这一段本来就是整数微秒级，
 *      用它最合适（它在关中断下可用，不依赖 FreeRTOS tick）。
 *  CCOUNT 读取用 esp_cpu_get_cycle_count()（内部是 asm volatile 读 CCOUNT，
 *  不会被编译器优化掉），CPU 周期/微秒由 esp_rom_get_cpu_ticks_per_us() 给出
 *  （S3 @240MHz → 240）。两个接口都在 esp_rom_sys.h / esp_cpu.h 里有据可查。
 *
 * ===========================================================================
 *  ⚠ 内存块大小 BSP_WS2812_MEM_BLOCK 的处理（重要）
 * ===========================================================================
 *  ESP32-S3 每个 RMT 通道只有 SOC_RMT_MEM_WORDS_PER_CHANNEL = 48 个字，
 *  而 rmt_new_tx_channel() 要求 mem_block_symbols 是偶数且 >= 48。
 *  驱动内部会把 mem_block_symbols 向上取整成整数个内存块
 *  （rmt_tx.c: rmt_tx_register_to_group()），一个通道占几块就吃掉几块内存。
 *  如果给 64 → 每通道占 2 块 → S3 的 4 个 TX 通道只能建起 2 路，
 *  4 路灯带会有 2 路拿不到通道（rmt_new_tx_channel 返回 ESP_ERR_NOT_FOUND）。
 *  所以 board_config.h 里已经把 BSP_WS2812_MEM_BLOCK 定成 48（恰好 1 块）。
 *  本文件仍然保留一道夹取保护：万一有人把它改大，这里会夹回 48 并告警，
 *  保证 4 路灯带始终能建起来。48 个 symbol 一个块足够用：
 *  编码器在 MEM_FULL 时会返回让驱动续传。
 *
 * 位宽说明：RMT 的 duration 字段只有 15 bit（最多 32767 tick），
 * 复位码 60us @10MHz = 600 tick，拆成两个符号各 300 tick，安全。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"      /* portDISABLE_INTERRUPTS / portENTER_CRITICAL */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_attr.h"               /* IRAM_ATTR */
#include "esp_cpu.h"                /* esp_cpu_get_cycle_count() */
#include "esp_rom_sys.h"            /* esp_rom_delay_us() / esp_rom_get_cpu_ticks_per_us() */

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#include "hal/gpio_ll.h"            /* gpio_ll_set_level()：位带要寄存器级写，够快 */
#include "soc/gpio_struct.h"        /* GPIO 实例 */
#include "soc/soc_caps.h"           /* SOC_RMT_MEM_WORDS_PER_CHANNEL */

#include "ws2812.h"
#include "board_config.h"

static const char *TAG = "WS2812";

/* ============================ 时序参数（ns） ============================ */
/* WS2812B：T0H=300ns T0L=900ns / T1H=900ns T1L=300ns / 复位 >50us
 * （两位相加 = 1.2us，落在 WS2812B 的 1.25us ±600ns 容差内） */
#define WS2812_T0H_NS           300
#define WS2812_T0L_NS           900
#define WS2812_T1H_NS           900
#define WS2812_T1L_NS           300
#define WS2812_RESET_NS         60000       /* >50us，留点余量 */

/* 位带后端的复位延时：整数微秒，正好用 esp_rom_delay_us() */
#define WS2812_BITBANG_RESET_US 60

/* S3 只有 4 个 RMT TX 通道 */
#define WS2812_MAX_STRIPS       4

/* RMT 发送队列深度 */
#define WS2812_TRANS_QUEUE_DEPTH    4

/* 发送完成等待的固定余量（ms），再加上按灯珠数估算的时间 */
#define WS2812_TX_TIMEOUT_MARGIN_MS 100
/* 每颗灯珠 24bit × 1.2us ≈ 30us */
#define WS2812_US_PER_LED           30

/* ns → CPU 周期，算不出来的极端情况下兜底用的 CPU 频率 */
#define WS2812_FALLBACK_TICKS_PER_US    240     /* S3 @240MHz */

/* ============================ 数据结构 ============================ */

/** 灯带句柄（ws2812.h 里是不透明指针，实体定义在这里） */
struct ws2812_strip_s {
    gpio_num_t              gpio;       /**< 数据脚 */
    uint32_t                led_num;    /**< 灯珠数量 */
    uint8_t                *buf;        /**< 像素缓冲，长度 = led_num*3，字节顺序 G,R,B */
    rmt_channel_handle_t    chan;       /**< RMT TX 通道 */
    rmt_encoder_handle_t    encoder;    /**< WS2812 自定义编码器 */
};

/**
 * @brief WS2812 自定义 RMT 编码器
 * @note base 必须是第一个字段（回调用 __containerof 反查）
 */
typedef struct {
    rmt_encoder_t           base;
    rmt_encoder_handle_t    bytes_encoder;  /**< 字节流 → 位符号（每 bit 一个 symbol） */
    rmt_encoder_handle_t    copy_encoder;   /**< 直接搬运复位符号 */
    int                     state;          /**< 状态机：0=发数据 1=发复位码 */
    rmt_symbol_word_t       reset_code;     /**< >50us 低电平复位码 */
} ws2812_encoder_t;

/* ============================ 静态状态 ============================ */

/* 已创建的灯带数量：受 S3 的 4 个 RMT TX 通道限制，用临界区保护 */
static portMUX_TYPE s_strip_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_strip_cnt = 0;

/* ============================ 工具函数 ============================ */

/**
 * @brief ns 换算成 RMT tick（1 tick = 1e9 / resolution_hz ns）
 * @note 驱动要求时长非 0，且符号里的 duration 字段只有 15 bit
 */
static uint32_t ws2812_ns_to_ticks(uint32_t ns, uint32_t res_hz)
{
    uint64_t ticks = ((uint64_t)ns * (uint64_t)res_hz) / 1000000000ULL;

    if (ticks == 0) {
        ticks = 1;              /* 0 时长会被硬件当成无效符号 */
    }
    if (ticks > 0x7FFFULL) {
        ticks = 0x7FFFULL;      /* duration0/duration1 各 15 bit */
    }
    return (uint32_t)ticks;
}

/**
 * @brief 算出这一芯片上"安全且刚好占 1 个内存块"的 mem_block_symbols
 *
 * 驱动要求：偶数，且 >= SOC_RMT_MEM_WORDS_PER_CHANNEL；
 * 同时每多占 1 个内存块就少一路 TX 通道 —— S3 上必须只有 1 块才能建 4 路。
 */
static size_t ws2812_mem_block_symbols(void)
{
    size_t block = (size_t)BSP_WS2812_MEM_BLOCK;
    const size_t one_block = (size_t)SOC_RMT_MEM_WORDS_PER_CHANNEL;

    if (block < one_block) {
        block = one_block;      /* 小于一个内存块会被驱动判为非法参数 */
    }
    if ((block & 0x1U) != 0U) {
        block++;                /* 驱动要求偶数 */
    }
    if (block > one_block) {
        block = one_block;      /* 多占内存块 = 少一路通道，夹到 1 块 */
    }
    return block;
}

/** @brief 一次发送的等待超时（ms） */
static int ws2812_tx_timeout_ms(uint32_t led_num)
{
    return (int)((led_num * WS2812_US_PER_LED) / 1000U) + WS2812_TX_TIMEOUT_MARGIN_MS;
}

/* ============================ RMT 编码器 ============================ */

/**
 * @brief 编码回调：先把 RGB(G,R,B) 字节流展开成位符号，最后补一个复位码
 *
 * 分块由驱动控制：内存块写满（RMT_ENCODING_MEM_FULL）就带着当前 state 返回，
 * 驱动腾出空间后会再用同一个 state 继续调用。
 */
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
    case 0:     /* 发 RGB 数据 */
        encoded_symbols += bytes_encoder->encode(bytes_encoder, tx_channel,
                                                 primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = 1;      /* 数据发完，切到复位码 */
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;                   /* 内存块满了，先让驱动去发 */
        }
        /* fall-through */
    case 1:     /* 发复位码（>50us 低电平） */
        encoded_symbols += copy_encoder->encode(copy_encoder, tx_channel,
                                                &ws_encoder->reset_code,
                                                sizeof(ws_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_encoder->state = RMT_ENCODING_RESET;     /* 整个会话结束，回到初始状态 */
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

/**
 * @brief 建一个 WS2812 编码器（bytes_encoder + copy_encoder + 复位码）
 * @param[in]  res_hz       RMT 分辨率（Hz），用来把 ns 换算成 tick
 * @param[out] ret_encoder  返回的编码器句柄
 */
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

    /* bit0 = 高 T0H + 低 T0L；bit1 = 高 T1H + 低 T1L
     * msb_first = 1：每个字节先发最高位；字节顺序由 buf 决定（G,R,B） */
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

    /* 复位码：一个符号里两段都是低电平，拼起来 = WS2812_RESET_NS（>50us） */
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

/* ============================ 位带后端 ============================ */

/**
 * @brief ns → CPU 周期（S3 @240MHz：1us = 240 周期）
 * @note 标 IRAM_ATTR：它被 IRAM 里的位带函数调用，万一没被内联也不能待在 flash，
 *       否则另一个核做 flash 操作关 cache 时会炸
 */
static inline IRAM_ATTR uint32_t ws2812_ns_to_cycles(uint32_t ns)
{
    uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();

    if (ticks_per_us == 0) {
        ticks_per_us = WS2812_FALLBACK_TICKS_PER_US;
    }
    return (uint32_t)(((uint64_t)ns * (uint64_t)ticks_per_us) / 1000ULL);
}

/**
 * @brief 位带方式把 len 颗灯珠的数据发出去（调用前必须已配好 GPIO 输出）
 *
 * 关中断区间只覆盖"数据位"部分（约 30us/颗）；
 * 复位延时放在开中断之后，不会长时间关中断。
 */
static void IRAM_ATTR ws2812_bitbang_send(gpio_num_t gpio, const uint8_t *grb, uint32_t len)
{
    const uint32_t t0h = ws2812_ns_to_cycles(WS2812_T0H_NS);
    const uint32_t t0l = ws2812_ns_to_cycles(WS2812_T0L_NS);
    const uint32_t t1h = ws2812_ns_to_cycles(WS2812_T1H_NS);
    const uint32_t t1l = ws2812_ns_to_cycles(WS2812_T1L_NS);
    const uint32_t t0_total = t0h + t0l;    /* 一个 bit 的总时长（1.2us） */
    const uint32_t t1_total = t1h + t1l;
    const uint32_t gpio_num = (uint32_t)gpio;
    const uint32_t bytes = len * 3U;

    portDISABLE_INTERRUPTS();

    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t byte = grb[i];

        /* 每个字节 MSB first */
        for (int bit = 7; bit >= 0; bit--) {
            /* 每次都以同一个时间原点起算，避免函数调用开销把位周期越拉越长
             * （读 CCOUNT 的循环不会被优化掉：RSR 指令是 asm volatile） */
            uint32_t start = esp_cpu_get_cycle_count();

            if (byte & (uint8_t)(1U << bit)) {
                gpio_ll_set_level(&GPIO, gpio_num, 1);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t1h) {
                    /* busy wait */
                }
                gpio_ll_set_level(&GPIO, gpio_num, 0);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t1_total) {
                    /* busy wait */
                }
            } else {
                gpio_ll_set_level(&GPIO, gpio_num, 1);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t0h) {
                    /* busy wait */
                }
                gpio_ll_set_level(&GPIO, gpio_num, 0);
                while ((uint32_t)(esp_cpu_get_cycle_count() - start) < t0_total) {
                    /* busy wait */
                }
            }
        }
    }

    /* 数据发完先拉低，再开中断 */
    gpio_ll_set_level(&GPIO, gpio_num, 0);
    portENABLE_INTERRUPTS();

    /* 复位：>50us 低电平（整数微秒，用 esp_rom_delay_us 正好） */
    esp_rom_delay_us(WS2812_BITBANG_RESET_US);
}

/* ============================ 对外接口 ============================ */

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

    /* 先占坑：S3 一共只有 4 个 RMT TX 通道 */
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
        .flags.init_level = 0,      /* 空闲时输出低电平（5.4.0 无此字段，默认即低） */
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

    /* WS2812/WS2812B 的字节顺序是 G,R,B */
    uint8_t *p = &h->buf[(size_t)index * 3U];
    p[0] = g;
    p[1] = r;
    p[2] = b;
    return ESP_OK;
}

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

esp_err_t ws2812_refresh(ws2812_strip_handle_t h)
{
    if (h == NULL || h->buf == NULL || h->chan == NULL || h->encoder == NULL) {
        ESP_LOGE(TAG, "refresh with invalid strip handle");
        return ESP_ERR_INVALID_ARG;
    }

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,            /* 不循环，只发一次 */
        .flags.eot_level = 0,       /* 发完保持低电平（灯带靠这个锁存颜色） */
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

uint32_t ws2812_get_num(ws2812_strip_handle_t h)
{
    if (h == NULL) {
        return 0;
    }
    return h->led_num;
}

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

    /* ★ GPIO 只在【第一次】用这个引脚时配置，之后不再重复配置。
     *   原因：gpio 驱动每次 gpio_reset_pin()/gpio_set_direction() 都会打一条 INFO 日志
     *        （"gpio: GPIO[48]| InputEn: 0| OutputEn: 0| ..."）。
     *   状态灯每闪一下就调一次本函数 → 每 150~500ms 刷一条日志，
     *   几秒钟就把串口刷满、把真正有用的日志淹掉。配置一次即可。 */
    static int s_bitbang_cfg_gpio = -1;
    esp_err_t err;

    if ((int)gpio != s_bitbang_cfg_gpio) {
        /* 这两步必须在关中断之前做：它们会校验参数并把 IO MUX 切到 GPIO 功能 */
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
