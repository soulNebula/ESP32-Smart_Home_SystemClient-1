// I²S 麦克风驱动
// 采样率 16KHz，单声道，16bit

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "i2s_mic.h"

static const char *TAG = "I2S_MIC";

// 可调参数
// 语音识别要这个速率
#define I2S_MIC_SAMPLE_RATE     16000
// 按 32 位读再取高位
#define I2S_MIC_SLOT_IS_32BIT   1
// 右移多少位
#define I2S_MIC_RAW_SHIFT       14

// 缓冲太小会丢帧
#define I2S_MIC_DMA_DESC_NUM    4
#define I2S_MIC_DMA_FRAME_NUM   240

// 看波动判断在不在
#define I2S_MIC_ALIVE_PEAK2PEAK_MIN     32

// 慢慢跟住直流偏置
#define I2S_MIC_DC_IIR_SHIFT    8

// 一次最多等这么久
#define I2S_MIC_READ_TIMEOUT_MS_MAX 2000

// 一次最多读这么多字
#define I2S_MIC_RAW_CHUNK       512

// 内部状态变量
static i2s_chan_handle_t s_rx_chan = NULL;
static bool              s_inited  = false;

// 跟住直流偏置
static int32_t s_dc_est = 0;

// 最近一次的电平快照
static i2s_mic_level_t s_level;

// 原始样本中转缓冲
static int32_t s_raw[I2S_MIC_RAW_CHUNK];

// 整数开平方
static uint32_t isqrt_u32(uint32_t x) {
    if (x == 0) {
        return 0;
    }
    // 先猜个大数再逼近
    // 从最大数往下试
    uint32_t r = 1u << 16;
    while (r > 0 && r > x / r) {
        r >>= 1;
    }
    // 再迭代几轮算准
    for (int i = 0; i < 6; i++) {
        if (r == 0) {
            break;
        }
        r = (r + x / r) / 2;
    }
    // 修掉最后一点误差
    while (r > 0 && r > x / r) { r--; }
    while ((r + 1) <= x / (r + 1)) { r++; }
    return r;
}

// 整数算对数
static int32_t log2_q8(uint32_t v) {
    if (v == 0) {
        // 零上面已经挡过
        return INT32_MIN;
    }

    int32_t e = 0;
    while (v >= 2u) {
        v >>= 1;
        e++;
    }

    // 把数归到一到二之间
    uint32_t m = v;
    int32_t frac = 0;
    for (int i = 0; i < 8; i++) {
        // 平方一次取一位
        m = (m * m) >> 16;
        frac <<= 1;
        if (m >= 2u * 65536u) {
            m >>= 1;
            frac |= 1;
        }
    }
    return (e << 8) + frac;
}

// 一趟采样的统计本
typedef struct {
    int32_t min;
    int32_t max;
    int64_t sum;
    int64_t sum_sq;
    int32_t peak;
    size_t  clip;
} mic_stat_t;

// 把统计清零
static void mic_stat_reset(mic_stat_t *st) {
    st->min    = INT32_MAX;
    st->max    = INT32_MIN;
    st->sum    = 0;
    st->sum_sq = 0;
    st->peak   = 0;
    st->clip   = 0;
}

// 转成十六位并记账
static void i2s_mic_accumulate(const int32_t *raw, size_t n, mic_stat_t *st, int16_t *out) {
    for (size_t i = 0; i < n; i++) {
        // 右移取高位
        int32_t v = (int32_t)(raw[i] >> I2S_MIC_RAW_SHIFT);

        // 减直流，别向下取整
        s_dc_est += (v - s_dc_est + (1 << (I2S_MIC_DC_IIR_SHIFT - 1))) >> I2S_MIC_DC_IIR_SHIFT;
        v -= s_dc_est;

        // 太大就削平防溢出
        if (v > I2S_MIC_FULL_SCALE) {
            v = I2S_MIC_FULL_SCALE;
            st->clip++;
        } else if (v < -I2S_MIC_FULL_SCALE - 1) {
            v = -I2S_MIC_FULL_SCALE - 1;
            st->clip++;
        }

        if (out != NULL) {
            out[i] = (int16_t)v;
        }

        if (v < st->min) { st->min = v; }
        if (v > st->max) { st->max = v; }
        st->sum    += v;
        st->sum_sq += (int64_t)v * (int64_t)v;

        const int32_t av = (v < 0) ? -v : v;
        if (av > st->peak) { st->peak = av; }
    }
}

// 算大小和分贝
static void i2s_mic_level_update(const mic_stat_t *st, size_t samples) {
    if (samples == 0) {
        // 没读到就只记次数
        s_level.read_count++;
        return;
    }

    const int64_t mean_sq = st->sum_sq / (int64_t)samples;
    s_level.rms = (int)((mean_sq > 0) ? isqrt_u32((uint32_t)mean_sq) : 0);

    s_level.peak         = (int)st->peak;
    s_level.dc           = (int)(st->sum / (int64_t)samples);
    s_level.peak_to_peak = (int)(st->max - st->min);
    s_level.last_samples = samples;
    s_level.clip         = (uint32_t)st->clip;
    s_level.read_count++;

    // 把峰值折成分贝
    if (st->peak > 0) {
        const int32_t l2_peak    = log2_q8((uint32_t)st->peak);
        // 满量程的对数值
        const int32_t l2_fs      = log2_q8(32768u);
        // 比满量程小就是负
        const int32_t diff_q8    = l2_peak - l2_fs;
        s_level.db_x10 = (int)((diff_q8 * 47) / 100);
    } else {
        s_level.db_x10 = -9990;
    }
}

// 把麦克风通道打开
esp_err_t i2s_mic_init(void) {
    // 开过就不再重开
    if (s_inited) {
        return ESP_OK;
    }

    // 建一个只收的通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // 缓冲少一点省内存
    chan_cfg.dma_desc_num  = I2S_MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_MIC_DMA_FRAME_NUM;
    // 没数据就填零
    chan_cfg.auto_clear    = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s（I²S 控制器可能被别的模块占用）",
                 esp_err_to_name(err));
        s_rx_chan = NULL;
        return err;
    }

    // 配成单声道十六千赫
#if I2S_MIC_SLOT_IS_32BIT
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_32BIT;
#else
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_16BIT;
#endif

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(slot_bits, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            // 它不用主时钟
            .mclk = I2S_GPIO_UNUSED,
            // 位时钟脚
            .bclk = BSP_I2S_MIC_SCK_GPIO,
            // 声道同步脚
            .ws   = BSP_I2S_MIC_WS_GPIO,
            // 只收不发
            .dout = I2S_GPIO_UNUSED,
            // 数据线
            .din  = BSP_I2S_MIC_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    // 开了它才吐数据
    // 头五十毫秒是垃圾
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    s_dc_est = 0;
    memset(&s_level, 0, sizeof(s_level));

    s_inited = true;
    ESP_LOGI(TAG, "init ok: %dHz / %dbit slot / mono  SCK=GPIO%d WS=GPIO%d SD=GPIO%d",
             I2S_MIC_SAMPLE_RATE, (int)slot_bits,
             (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
    ESP_LOGI(TAG, "INMP441 是 I²S【不是 I²C】: L/R->GND, VDD->3V3(不要5V); "
                  "麦克风没接时不影响系统，串口敲 `mic` 可看实时电平");
    return ESP_OK;
}

// 读一段单声道采样
esp_err_t i2s_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms) {
    if (out_read != NULL) {
        *out_read = 0;
    }
    if (buf == NULL || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || s_rx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms > I2S_MIC_READ_TIMEOUT_MS_MAX) {
        timeout_ms = I2S_MIC_READ_TIMEOUT_MS_MAX;
    }

    // 超时按总预算切段
    const int64_t t_start   = esp_timer_get_time();
    const int64_t budget_us = (int64_t)timeout_ms * 1000LL;

    mic_stat_t st;
    mic_stat_reset(&st);

    size_t    got = 0;
    esp_err_t err = ESP_OK;

    while (got < samples) {
        // 一次读两倍字再抽
        size_t want = samples - got;
        if (want > I2S_MIC_RAW_CHUNK / 2) {
            want = I2S_MIC_RAW_CHUNK / 2;
        }
        // 字数是采样两倍
        const size_t want_raw = want * 2;

        // 零就是不等待
        TickType_t wait_ticks = 0;
        if (budget_us > 0) {
            const int64_t left_us = budget_us - (esp_timer_get_time() - t_start);
            if (left_us <= 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            // 至少等一拍
            wait_ticks = pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000));
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
        }

        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, s_raw, want_raw * sizeof(int32_t), &bytes_read, wait_ticks);
        if (err != ESP_OK) {
            // 超时不算错，往上抛
            break;
        }

        const size_t n = bytes_read / sizeof(int32_t);
        if (n == 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        // 只留麦克风那半时隙
        size_t nd = 0;
        for (size_t i = 0; i + 1 < n; i += 2) {
            s_raw[nd++] = s_raw[i];
        }
        if (nd == 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        i2s_mic_accumulate(s_raw, nd, &st, buf + got);
        got += nd;
    }

    i2s_mic_level_update(&st, got);

    if (got > 0 && st.clip > 0) {
        // 削顶多了就提醒
        static int64_t s_last_clip_log_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_last_clip_log_us > 5000000LL) {
            s_last_clip_log_us = now;
            ESP_LOGW(TAG, "削顶 %u 次（peak=%d）：若持续出现，把 i2s_mic.c 的 "
                          "I2S_MIC_RAW_SHIFT 从 %d 调大到 %d",
                     (unsigned)st.clip, (int)st.peak,
                     I2S_MIC_RAW_SHIFT, I2S_MIC_RAW_SHIFT + 2);
        }
    }

    if (out_read != NULL) {
        *out_read = got;
    }

    // 读满就算成功
    if (got == samples) {
        return ESP_OK;
    }
    return (err == ESP_OK) ? ESP_ERR_TIMEOUT : err;
}

void i2s_mic_read_level(i2s_mic_level_t *out) {
    if (out == NULL) {
        return;
    }
    // 不加锁，快照够用
    *out = s_level;
}

// 看麦克风在不在
bool i2s_mic_is_ready(void) {
    if (!s_inited || s_rx_chan == NULL) {
        return false;
    }
    if (s_level.read_count == 0) {
        // 一次都没读到
        return false;
    }
    // 看波动大小判断
    return (s_level.peak_to_peak > I2S_MIC_ALIVE_PEAK2PEAK_MIN);
}

// 采一段打印统计
void i2s_mic_dump(int seconds) {
    if (!s_inited || s_rx_chan == NULL) {
        printf("  [mic] I²S 未初始化，先确认 i2s_mic_init() 的日志\n");
        return;
    }
    if (seconds < 1)  { seconds = 1; }
    if (seconds > 10) { seconds = 10; }

    printf("  [mic] 采集 %d 秒原始音频（16KHz/16bit/单声道），每 500ms 一行统计：\n", seconds);
    printf("  [mic] min/max=瞬时极值  avg=直流偏置  rms=有效值  peak=峰值  clip=削顶次数\n");
    printf("  [mic] 判据：安静时 rms 几十~几百 且 min/max 在动 = 麦在工作；\n");
    printf("  [mic]       整行全 0 = 没接；min=max 恒定 = 时序/声道接错\n");

    // 这是半秒的点数
    const size_t chunk = 8000;
    int16_t *buf = (int16_t *)malloc(chunk * sizeof(int16_t));
    if (buf == NULL) {
        printf("  [mic] 内存不足，dump 取消\n");
        return;
    }

    const int rounds = seconds * 2;
    int32_t g_min = INT32_MAX, g_max = INT32_MIN, g_peak = 0;

    for (int r = 0; r < rounds; r++) {
        size_t got = 0;
        const esp_err_t err = i2s_mic_read(buf, chunk, &got, 1000);
        if (got == 0) {
            printf("  [mic] t=%4dms  读取失败(%s)，无数据 —— 麦克风可能没接\n",
                   r * 500, esp_err_to_name(err));
            continue;
        }

        int32_t mn = INT32_MAX, mx = INT32_MIN, pk = 0;
        int64_t sum = 0;
        size_t  clip = 0;
        for (size_t i = 0; i < got; i++) {
            const int32_t v = buf[i];
            if (v < mn) { mn = v; }
            if (v > mx) { mx = v; }
            sum += v;
            const int32_t av = (v < 0) ? -v : v;
            if (av > pk) { pk = av; }
            if (av >= I2S_MIC_FULL_SCALE) { clip++; }
        }
        if (mn < g_min) { g_min = mn; }
        if (mx > g_max) { g_max = mx; }
        if (pk > g_peak) { g_peak = pk; }

        printf("  [mic] t=%4dms  min=%6d max=%6d avg=%6d peak=%6d clip=%u\n",
               r * 500, (int)mn, (int)mx, (int)(sum / (int64_t)got), (int)pk, (unsigned)clip);
    }

    printf("  [mic] 合计: min=%d max=%d 峰峰值=%d peak=%d 累计读取=%" PRIu32 " 次\n",
           (int)g_min, (int)g_max, (int)(g_max - g_min), (int)g_peak, s_level.read_count);

    if (g_peak <= I2S_MIC_ALIVE_PEAK2PEAK_MIN) {
        printf("  [mic] 结论: 【没采到有效音频】—— 检查 SD/SCK/WS 三根线、"
               "L/R 是否接了 GND、VDD 是否 3.3V\n");
    } else if ((g_max - g_min) <= I2S_MIC_ALIVE_PEAK2PEAK_MIN) {
        printf("  [mic] 结论: 【数据恒定不动】—— 像时序/声道接错，核对 SCK 与 WS 有没有接反\n");
    } else {
        printf("  [mic] 结论: 【麦克风在工作】—— 现在对着麦克风说话，peak 应明显变大\n");
    }

    free(buf);
}
