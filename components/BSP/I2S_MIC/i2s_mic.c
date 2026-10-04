/**
 * @file  i2s_mic.c
 * @brief INMP441 I²S 数字麦克风驱动实现（ESP-IDF 5.4 新版 I2S 驱动）
 *
 * ===========================================================================
 *  ★★ 接线：INMP441 是 I²S，不是 I²C ★★
 * ===========================================================================
 *  很多人第一眼会把它当 I²C（毕竟本工程 OLED/SHT30/BH1750 都挂 I²C），
 *  于是往 SDA/SCL 上插 —— 结果是"程序不报错、麦克风永远没数据"。
 *  INMP441 的三根信号线是 I²S 的：
 *      SCK(BCLK) 位时钟   → BSP_I2S_MIC_SCK_GPIO  = GPIO13（扩展板左排 IO13）
 *      WS(LRCLK) 帧同步   → BSP_I2S_MIC_WS_GPIO   = GPIO39（扩展板右排 IO39）
 *      SD        串行数据 → BSP_I2S_MIC_SD_GPIO   = GPIO40（扩展板右排 IO40）
 *      L/R       声道选择 → GND（单麦必须固定接一端！悬空=左右乱跳/无声）
 *      VDD                → 3.3V（★ 不是 5V，INMP441 是 3.3V 器件）
 *  I²S 没有从机地址，靠 WS 的高低电平分左右声道，所以三根线【必须全接对】。
 *
 * ===========================================================================
 *  【设计要点】
 * ===========================================================================
 *  1) 用 IDF 5.x 的【新版通用 I2S 驱动】（driver/i2s_std.h）：
 *       i2s_new_channel() → i2s_channel_init_std_mode() → i2s_channel_enable()
 *     不用 IDF 4.x 那套 i2s_driver_install()/i2s_config_t 老 API（5.x 已废弃）。
 *  2) 格式钉死 16KHz / 16bit 单声道 —— 这是 ESP-SR（AFE + WakeNet + MultiNet）
 *     对输入的硬性要求，所以不暴露给调用方，避免上层配错导致"能出声但识别不了"。
 *  3) ESP32-S3 做 I²S 主机：SCK/WS 由 ESP32 产生，麦克风只在自己的 SD 脚上
 *     按节拍吐数据。单麦 + 无回采参考 → 不需要 AEC 的参考通道，RX 单工即可。
 *  4) 缺件只告警不崩：见 i2s_mic_is_ready() 的判据说明。
 *  5) 全程【不用浮点】：本工程可能打开 CONFIG_NEWLIB_NANO_FORMAT，printf 家族
 *     会把 %f 打成空串，而且软件浮点在这里纯属浪费 CPU。
 *     RMS 用整数牛顿迭代开方，dBFS 用 Q8 定点 log2 递推（见下面两个函数）。
 *
 * ===========================================================================
 *  ★ 32bit 时隙 vs 16bit 时隙 —— 两种写法都试过，这里为什么选 32bit
 * ===========================================================================
 *  INMP441 内部是 24bit ADC，输出是"24bit 数据 + 8bit 补零"，即
 *  【在一个 32bit 时隙里高位对齐】，永远是 32 个位时钟一帧。
 *  （另外：上电后前 ~50ms 输出无效数据，属于正常现象，与配置无关。）
 *
 *  写法 A（本驱动采用）：按 I2S_DATA_BIT_WIDTH_32BIT 配，收到 32bit 后
 *      自己右移取高位。优点：与真实时序完全一致，不会因位宽不符产生"半个
 *      采样"错位；缺点：要自己移位 + 处理直流偏置。
 *
 *  写法 B：把位宽直接配成 I2S_DATA_BIT_WIDTH_16BIT。多数情况下 ESP32 的
 *      I²S 外机会自动把 32bit 时隙裁成 16bit 高位给到 DMA，也能出声；但它
 *      依赖"槽位裁剪"行为，某些 IDF 版本/时钟配置下会变成读到低位字节
 *      （结果是巨大的固定噪声、或恒定错误直流值），排错非常困难。
 *      → 所以【不推荐】把 16bit 槽位当成默认，除非你在真机上实测确认信噪比正常。
 *      真要试写法 B：把下面的 I2S_MIC_SLOT_IS_32BIT 改成 0 即可，
 *      同时把 I2S_MIC_RAW_SHIFT 改成 0（16bit 数据已经在 int16 量级里）。
 *
 *  移位量的由来（INMP441 + 写法 A）：
 *      原始 32bit： [ 24bit 有效数据 ][ 8bit 0 ]
 *      >>8        ： [ 24bit 有效数据 ]               ← 得到有符号 24bit
 *      >>6        ： [ 18bit 有效数据 ]               ← 缩到 16bit 量级（留 2bit 余量）
 *      → 一共右移 14 位。留 2bit 余量是有意的：INMP441 满量程不会真打到 24bit
 *        满幅（正常声压级下大约只用到 -20dBFS），右移 14 而不是 16 可以让
 *        有用信号幅度大 4 倍，提高 ESP-SR 的输入信噪比，同时不会削顶。
 *      如果你发现日志里"削顶 N 次"明显增长，把 I2S_MIC_RAW_SHIFT 改成 16。
 *
 * ===========================================================================
 *  ★★ I²S 实际采样率是配置的 2 倍（2026-10-02 实测定位，唤醒词不响的真因）
 * ===========================================================================
 *  症状：唤醒词「你好小智」喊几十遍只偶尔响一次；日志里喂帧速率稳定
 *  199 帧/秒（160 点/帧），而 16KHz 麦克风的物理上限是 100 帧/秒 ——
 *  按累计计数核算，I²S 实际以 ≈32,000 字/秒交付数据。
 *
 *  根因（读 IDF 5.4.4 esp_driver_i2s/i2s_std.c 证实）：
 *      i2s_std_set_slot() 里 `handle->total_slot = 2;` 是【硬编码】的，
 *      与 slot_mode 无关。于是 Philips 模式下每个 WS 周期固定两个 32bit
 *      时隙：bclk = 16000 × 2 × 32 = 1.024MHz，WS = bclk / 64 = 16KHz，
 *      但 DMA 按【时隙速率】= 32,000 字/秒 把两个时隙全部装进缓冲。
 *      INMP441 的 L/R 接 GND 只在左时隙（WS 低）输出，右时隙是悬空/保持
 *      电平 —— 所以流是 [左=麦克风][右=垃圾] 交错，2 个字只有 1 个有效。
 *
 *  后果：把 32000 字/秒当成 16000 采样/秒喂给 ESP-SR，音频时间被拉伸
 *  2 倍（"你好小智"变成慢动作），WakeNet 的特征完全对不上 → 永不触发。
 *  这也能解释早期排错里所有的怪现象（199 帧/秒"达标"、直流偏置异常、
 *  波形不对称）——它们不是三个 bug，是同一个 bug 的三个面。
 *
 *  修法（本驱动）：读【2 倍原始字】，只取偶数下标（左时隙）做抽取，
 *  得到真正的 16KHz 单声道流。单次驱动读上限 = I2S_MIC_RAW_CHUNK/2 有效采样。
 *  这是软件侧修复；硬件侧本来就没有别的接法（INMP441 就是双时隙器件）。
 * ===========================================================================
 */
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

/* -------------------------------------------------------------------------- */
/*  参数                                                                       */
/* -------------------------------------------------------------------------- */
#define I2S_MIC_SAMPLE_RATE     16000   /* ESP-SR 硬性要求：16KHz */
#define I2S_MIC_SLOT_IS_32BIT   1       /* 1 = 32bit 时隙读 + 取高位（推荐，见文件头） */
#define I2S_MIC_RAW_SHIFT       14      /* 32bit 原始值 → int16 的右移位数（见文件头） */

/* DMA：一帧 240 点 @16KHz = 15ms；4 个缓冲 ≈ 60ms 总延迟，够 AFE 用又不占内存。
 * 为什么不能太小：I²S 是等时流，缓冲太小会在 WiFi/BLE 抢占 CPU 时丢帧，
 * 丢帧会让 AFE 的 VAD/唤醒词丢精度（表现为"喊了没反应"）。 */
#define I2S_MIC_DMA_DESC_NUM    4
#define I2S_MIC_DMA_FRAME_NUM   240

/* 判"麦克风在不在"的最小峰峰值（int16 计数）：
 * 没接麦克风 / L-R 悬空 / SCK-WS 接反 → 数据恒定（全 0 或某个固定直流值），
 * 峰峰值 ≈ 0。正常麦克风在【安静房间】里光是底噪峰峰值也有几十~几百。
 * 取 32 是个很保守的门限：高于它就一定不是"恒定直流"。 */
#define I2S_MIC_ALIVE_PEAK2PEAK_MIN     32

/* 直流偏置的一阶跟踪系数（约 1/256 ≈ 0.4%）：
 * INMP441 的输出直流偏置本身很小（几十个计数），但个别模块/供电不稳时能到
 * 几百。跟得太快会把低频语音一起滤掉（语音基频可低到 80Hz），跟得太慢又会
 * 让直流残留进 AFE。1/256 在 16KHz 下对应约 0.26Hz 截止，安全。 */
#define I2S_MIC_DC_IIR_SHIFT    8

/* i2s_channel_read 单次最长等待（毫秒），防止调用方传个天荒地老 */
#define I2S_MIC_READ_TIMEOUT_MS_MAX 2000

/* 一次最多处理的采样点数（中转缓冲大小）。
 * ★ 注意单位是【原始 32bit 字】：每个 WS 周期有左右两个时隙，
 *   驱动读回来的是双时隙流，2 个字才能抽 1 个有效采样（见文件头"2x"一节），
 *   所以单次驱动读最多换算 I2S_MIC_RAW_CHUNK/2 个有效采样。 */
#define I2S_MIC_RAW_CHUNK       512

/* -------------------------------------------------------------------------- */
/*  内部状态                                                                   */
/* -------------------------------------------------------------------------- */
static i2s_chan_handle_t s_rx_chan = NULL;
static bool              s_inited  = false;

/** 直流偏置跟踪值（int32，避免累加溢出） */
static int32_t s_dc_est = 0;

/** 最近一次读取的电平快照（i2s_mic_read 写，i2s_mic_read_level 读） */
static i2s_mic_level_t s_level;

/** 32bit 原始样本中转缓冲 */
static int32_t s_raw[I2S_MIC_RAW_CHUNK];

/* -------------------------------------------------------------------------- */
/*  整数数学：RMS 开方 + Q8 定点 log2（全程不用浮点，见文件头第 5 条）        */
/* -------------------------------------------------------------------------- */

/** @brief 整数平方根（牛顿迭代），x < 0 时返回 0 */
static uint32_t isqrt_u32(uint32_t x)
{
    if (x == 0) {
        return 0;
    }
    /* 初值取 2 的整数次幂上界，牛顿迭代 4~5 次就收敛到整数精度 */
    uint32_t r = 1u << 16;              /* 最大支持 x < 2^32 */
    while (r > 0 && r > x / r) {
        r >>= 1;
    }
    /* 此时 r ≈ sqrt(x) 的粗值；再做几轮牛顿迭代 */
    for (int i = 0; i < 6; i++) {
        if (r == 0) {
            break;
        }
        r = (r + x / r) / 2;
    }
    /* 修正 ±1 的余差 */
    while (r > 0 && r > x / r) { r--; }
    while ((r + 1) <= x / (r + 1)) { r++; }
    return r;
}

/**
 * @brief 以 2 为底的对数，Q8 定点（返回值 = log2(v) * 256）
 * @param v 必须 > 0
 *
 * 递推原理：把 v 归一化到 [1,2)，整数部分就是右移次数；
 * 小数部分每轮平方一次取整数位，得到 1 个二进制小数位（共 8 位）。
 * 整数实现的好处：不需要 libm、不受 NANO_FORMAT 影响、速度稳定。
 */
static int32_t log2_q8(uint32_t v)
{
    if (v == 0) {
        return INT32_MIN;       /* 调用方已对 0 做过特判 */
    }

    int32_t e = 0;
    while (v >= 2u) {
        v >>= 1;
        e++;
    }

    uint32_t m = v;             /* m ∈ [1,2)，用 Q16 表示：实际值 = m / 65536 */
    int32_t frac = 0;
    for (int i = 0; i < 8; i++) {
        m = (m * m) >> 16;      /* 平方后 m ∈ [1,4) */
        frac <<= 1;
        if (m >= 2u * 65536u) {
            m >>= 1;
            frac |= 1;
        }
    }
    return (e << 8) + frac;
}

/* -------------------------------------------------------------------------- */
/*  内部函数                                                                   */
/* -------------------------------------------------------------------------- */

/** 单趟统计上下文（避免函数参数长到看不清） */
typedef struct {
    int32_t min;
    int32_t max;
    int64_t sum;
    int64_t sum_sq;
    int32_t peak;
    size_t  clip;
} mic_stat_t;

static void mic_stat_reset(mic_stat_t *st)
{
    st->min    = INT32_MAX;
    st->max    = INT32_MIN;
    st->sum    = 0;
    st->sum_sq = 0;
    st->peak   = 0;
    st->clip   = 0;
}

/**
 * @brief 把 32bit 原始样本转成 int16（含直流偏置补偿），同时累计统计量
 * @param out 可为 NULL（只统计不输出）
 */
static void i2s_mic_accumulate(const int32_t *raw, size_t n, mic_stat_t *st, int16_t *out)
{
    for (size_t i = 0; i < n; i++) {
        /* 1) 取高位得到 int16 量级（见文件头"移位量的由来"） */
        int32_t v = (int32_t)(raw[i] >> I2S_MIC_RAW_SHIFT);

        /* 2) 慢速跟踪直流偏置并减掉 —— 不让直流吃掉 AFE 的动态范围。
         * ★ 必须【四舍五入】而不是直接 >>：C 的算术右移对负数向下取整，
         *   每步平均多减一点点，跟踪器被缓慢拽向负值，最后给输出整体
         *   抬高一个假直流（真机实测：输出平均 +135，min 几乎全为正）。
         *   +128 再 >> 就是 round-to-nearest，正负差值都能对称修正。 */
        s_dc_est += (v - s_dc_est + (1 << (I2S_MIC_DC_IIR_SHIFT - 1))) >> I2S_MIC_DC_IIR_SHIFT;
        v -= s_dc_est;

        /* 3) 限幅（防止极端情况下溢出 int16 后翻转成反向大噪声） */
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

/** @brief 由统计量算出 RMS / dBFS / 峰峰值并写入 s_level */
static void i2s_mic_level_update(const mic_stat_t *st, size_t samples)
{
    if (samples == 0) {
        /* 一点都没读到：只累加计数，不改电平（保留上一次的值供诊断） */
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

    /* dBFS = 20*log10(peak / 32768) = 6.0206 * log2(peak / 32768)
     * log2(x) 用 Q8 定点算，再乘 6.0206/32768*256*10 ≈ 0.4705 → 用 47/100 近似，
     * 结果就是 dBFS × 10 的整数（例如 -420 = -42.0 dBFS）。
     * 峰值为 0（无信号）→ 写成 -9990 这个哨兵值，打印时一眼能认出来。 */
    if (st->peak > 0) {
        const int32_t l2_peak    = log2_q8((uint32_t)st->peak);
        const int32_t l2_fs      = log2_q8(32768u);          /* = 15 * 256 = 3840 */
        const int32_t diff_q8    = l2_peak - l2_fs;          /* 负数（peak < 满量程） */
        s_level.db_x10 = (int)((diff_q8 * 47) / 100);
    } else {
        s_level.db_x10 = -9990;
    }
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */
esp_err_t i2s_mic_init(void)
{
    /* 幂等：重复调用直接返回，不会重装驱动 */
    if (s_inited) {
        return ESP_OK;
    }

    /* ---- 1. 建 I²S 通道（只用 RX：单麦，不需要回采参考通道） ----
     * I2S_NUM_AUTO 让 IDF 自己挑一个空闲控制器（本工程没有别的 I²S 外设）。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* 宏展开后 dma_desc_num=6 / dma_frame_num=240，这里按语音场景重设描述符数：
     * 4 个缓冲 ≈ 60ms 总延迟，比默认更省内存、延迟更低。 */
    chan_cfg.dma_desc_num  = I2S_MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_MIC_DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;   /* 没数据时填 0，而不是重复上一次的旧数据 */

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s（I²S 控制器可能被别的模块占用）",
                 esp_err_to_name(err));
        s_rx_chan = NULL;
        return err;
    }

    /* ---- 2. 配标准（Philips）模式：16KHz / 单声道 ---- */
#if I2S_MIC_SLOT_IS_32BIT
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_32BIT;
#else
    const i2s_data_bit_width_t slot_bits = I2S_DATA_BIT_WIDTH_16BIT;
#endif

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(slot_bits, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,            /* INMP441 不需要 MCLK */
            .bclk = BSP_I2S_MIC_SCK_GPIO,       /* SCK / 位时钟 */
            .ws   = BSP_I2S_MIC_WS_GPIO,        /* WS  / 帧同步 */
            .dout = I2S_GPIO_UNUSED,            /* 只收不发 */
            .din  = BSP_I2S_MIC_SD_GPIO,        /* SD  / 麦克风数据 */
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

    /* ---- 3. 使能通道：使能后 SCK/WS 立即开始输出，麦克风同时开始吐数据 ----
     * ⚠ INMP441 上电后前 ~50ms 的输出是无效的（内部滤波器未收敛），
     *   这段时间读到的是垃圾值。ESP-SR 的 AFE 有自己的启动丢弃逻辑，
     *   所以这里不用特别处理，只是记一笔免得以后当成 bug 排查。 */
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

esp_err_t i2s_mic_read(int16_t *buf, size_t samples, size_t *out_read, uint32_t timeout_ms)
{
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

    /* 总的超时预算：切成若干段读，每段只给"剩下的预算"，避免"每段都给满超时"
     * 导致实际等待时间被放大 N 倍。 */
    const int64_t t_start   = esp_timer_get_time();
    const int64_t budget_us = (int64_t)timeout_ms * 1000LL;

    mic_stat_t st;
    mic_stat_reset(&st);

    size_t    got = 0;
    esp_err_t err = ESP_OK;

    while (got < samples) {
        /* ★ 每次驱动读要 2 倍原始字：一个 WS 周期含左右两个 32bit 时隙，
         *   抽取后只剩一半（见文件头"★ I²S 实际采样率是配置的 2 倍"一节） */
        size_t want = samples - got;
        if (want > I2S_MIC_RAW_CHUNK / 2) {
            want = I2S_MIC_RAW_CHUNK / 2;
        }
        const size_t want_raw = want * 2;   /* 原始字 = 2 × 有效采样 */

        TickType_t wait_ticks = 0;      /* 0 = 不等待（非阻塞） */
        if (budget_us > 0) {
            const int64_t left_us = budget_us - (esp_timer_get_time() - t_start);
            if (left_us <= 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            /* 向上取整到 1 tick，避免 left_us < 1 tick 时退化成"不等待" */
            wait_ticks = pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000));
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
        }

        size_t bytes_read = 0;
        err = i2s_channel_read(s_rx_chan, s_raw, want_raw * sizeof(int32_t), &bytes_read, wait_ticks);
        if (err != ESP_OK) {
            /* 超时 = "麦克风没数据/没接"的正常表现，不当错误刷屏，往上抛由调用方处理 */
            break;
        }

        const size_t n = bytes_read / sizeof(int32_t);
        if (n == 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        /* ★ 抽取麦克风时隙（偶数下标）★
         * 2026-10-02 实测（两次烧录对照）：
         *   · 偶数下标：底噪 ±120、峰峰值 200+，是【有信号】的时隙（麦克风）；
         *   · 奇数下标：恒定 ~0（峰峰值 3），是悬空时隙（SD 脚无驱动）。
         * 中间一度误判奇偶反转（把"说话时电平低"归因于时隙取反），翻转后
         * mic 自检直接报"没采到音频"，翻回偶数恢复。真正的"说话电平低"
         * 是当时的测试距离问题。原地压缩后统一走 accumulate。 */
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
        /* 削顶限速打日志：说明移位量偏大或音量过大，日志里给出调整依据 */
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

    /* 读满了就不算错（即使中途出现过一次超时重试） */
    if (got == samples) {
        return ESP_OK;
    }
    return (err == ESP_OK) ? ESP_ERR_TIMEOUT : err;
}

void i2s_mic_read_level(i2s_mic_level_t *out)
{
    if (out == NULL) {
        return;
    }
    /* 结构里全是标量，32 位对齐读写在 ESP32-S3 上是原子的；
     * 这里只求"快照大致一致"，不追求跨字段严格一致，所以不加锁。 */
    *out = s_level;
}

bool i2s_mic_is_ready(void)
{
    if (!s_inited || s_rx_chan == NULL) {
        return false;
    }
    if (s_level.read_count == 0) {
        return false;       /* 还一次都没读到过数据 */
    }
    /* ★ 判据用【峰峰值】而不是 RMS，也不是原始直流：
     *   · 麦克风完全没接（SD 悬空被拉低）→ 全 0 → 峰峰值 = 0
     *   · L/R 悬空 / SCK-WS 接反 → 读回来是恒定直流 → 直流被 s_dc_est 减掉后
     *     波动量 ≈ 0 → 峰峰值也接近 0（哪怕原始直流很大）
     *   · 正常工作的麦克风 → 光是底噪就有几十~几百的波动 → 峰峰值明显超门限
     * 所以 peak_to_peak > I2S_MIC_ALIVE_PEAK2PEAK_MIN 是"真的在拾音"的有力证据。 */
    return (s_level.peak_to_peak > I2S_MIC_ALIVE_PEAK2PEAK_MIN);
}

void i2s_mic_dump(int seconds)
{
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

    const size_t chunk = 8000;      /* 500ms @16KHz */
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
