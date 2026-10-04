/**
 * @file  adc_bus.c
 * @brief ADC1 单次采样 + 校准实现 —— 光敏电阻（GPIO1，ADC1_CH0）/ 雨滴 AO（GPIO2，ADC1_CH1）
 *
 * ============================ 设计要点 ============================
 * 1) 只用 esp_adc 组件的"单次采样"驱动：adc_oneshot_new_unit() +
 *    adc_oneshot_config_channel() + adc_oneshot_read()。
 *    ESP32-S3 上 WiFi 一开 ADC2 就不可用，所以本工程只用 ADC1。
 * 2) 校准优先用曲线拟合（S3 只支持曲线拟合：adc_cali_schemes.h 里
 *    ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED = 1，线拟合的宏在该芯片上
 *    根本没定义，所以绝对不能在代码里引用 line_fitting 的 API）。
 *    eFuse 没烧校准数据时 create 会失败 → 校准句柄置 NULL，并置静态标志
 *    s_cali_valid = false，read_mv 走线性近似兜底，绝不因为没校准就罢工。
 * 3) 幂等：重复调用 adc_bus_init() 直接返回 ESP_OK。
 * 4) 通道 / 衰减 / 位宽全部取自 board_config.h，不出现硬编码。
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"          /* 失败日志限速用 esp_timer_get_time() */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"           /* adc_cali_handle_t / adc_cali_raw_to_voltage */
#include "esp_adc/adc_cali_scheme.h"    /* adc_cali_create_scheme_curve_fitting() + 支持宏 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "adc_bus.h"    /* 含 board_config.h */

static const char *TAG = "ADC_BUS";

/* ===========================================================================
 *  ★ 为什么这里必须自己加一把锁（2026-09-28 实机踩坑记录）
 * ===========================================================================
 * IDF 的 adc_oneshot_read() 内部用的是【非阻塞 try-lock】：
 *
 *     if (adc_lock_try_acquire(handle->unit_id) != ESP_OK) {
 *         return ESP_ERR_TIMEOUT;            // 拿不到锁立刻失败，不等待
 *     }
 *     ...
 *     return valid ? ESP_OK : ESP_ERR_TIMEOUT;
 *
 * 本工程有两个任务共读 ADC1：
 *     · 五位键盘任务   每 10ms 读 CH9（一次 4 个采样，约 400 次/秒）
 *     · 传感器任务     每 1000ms 读 CH0/CH1（一次 16 个采样）
 * 两者撞上时，抢输的一方【整批采样全部失败】，实机日志为证：
 *     E ADC_BUS: all 4 samples failed on channel 9      ← 键盘侧，每分钟十几次
 *     E ADC_BUS: all 16 samples failed on channel 0     ← 光照侧，约每分钟 1~2 次
 *     W SENSOR: 光照 ADC 读取失败，light_mv 保留上次值
 * 键盘任务 100Hz 抢锁，会把 16 连采的传感器饿死（livelock）。
 *
 * 解决办法：在 adc_bus 这一层用【自己的递归互斥锁】把所有 ADC 访问串行化，
 * 让 IDF 那把 try-lock 永远不产生竞争；再加少量重试兜底。
 * =========================================================================== */
static SemaphoreHandle_t s_adc_mutex = NULL;   /* 递归锁：read_raw/mv/mv_avg 会嵌套调用 */
#define ADC_READ_RETRY          3              /* 单次采样失败时的重试次数（仅 TIMEOUT 重试） */

/* 12bit 满量程原始值 */
#define ADC_RAW_FULL_SCALE      4095

/* 12dB 衰减时 ADC 量程约 0~3100mV（board_config.h 注释里也这么写）。
 * 这是个"兜底"近似：只有在曲线拟合校准不可用时才用得上，
 * 精度大约 ±100mV，对光敏/雨滴这种阈值型判断完全够用。 */
#define ADC_LINEAR_FULL_SCALE_MV    3100

/* 平均值的采样次数上限：调用方万一传个 100000，不至于把调用任务卡上几秒 */
#define ADC_AVG_SAMPLES_MAX     256
#define ADC_AVG_SAMPLES_DEFAULT 16

/* ------------------------------------------------------------------ */
/*  单例状态                                                           */
/* ------------------------------------------------------------------ */
static adc_oneshot_unit_handle_t s_unit = NULL;     /* ADC1 单次采样单元 */
static adc_cali_handle_t         s_cali = NULL;     /* 校准句柄，失败为 NULL */
static bool s_cali_valid = false;                   /* 静态标志：校准是否可用 */
static bool s_inited = false;                       /* 幂等标志 */

/* ------------------------------------------------------------------ */
/*  内部函数                                                           */
/* ------------------------------------------------------------------ */

/** @brief 建 ADC1 单元 + 配两个通道（内部使用，不对外） */
static esp_err_t adc_bus_setup_unit_and_channels(void)
{
    if (s_unit == NULL) {
        /* clk_src 留 0 = 用驱动默认时钟源（adc_oneshot.c 里 0 会走
         * ADC_DIGI_CLK_SRC_DEFAULT），ulp_mode 必须显式关掉 */
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = BSP_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };

        esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
        if (err != ESP_OK) {
            s_unit = NULL;
            /* adc_oneshot_new_unit() 在"单元已被占用"时返回的是
             * ESP_ERR_NOT_FOUND（源码里的日志是 "adc%d is already in use"），
             * 部分分支/版本会返回 ESP_ERR_INVALID_STATE，两种都当作
             * "ADC1 已经初始化过"处理。
             * 但 IDF 没有公开 API 能取回别人手里的 unit 句柄，所以这里只能
             * 标记为已初始化、并明确警告读操作会失败。 */
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "ADC unit already claimed by another module (%s); "
                              "adc_bus_read_raw/read_mv will fail — 请让其它模块"
                              "统一走 adc_bus_init()，不要自己调 adc_oneshot_new_unit()",
                         esp_err_to_name(err));
                s_inited = true;
                return ESP_OK;
            }
            ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "ADC%d unit created (atten=%d, bitwidth=%d)",
                 (int)BSP_ADC_UNIT + 1, (int)BSP_ADC_ATTEN, (int)BSP_ADC_BITWIDTH);
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BSP_ADC_ATTEN,
        .bitwidth = BSP_ADC_BITWIDTH,
    };

    esp_err_t err = adc_oneshot_config_channel(s_unit, BSP_ADC_CH_LIGHT, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config LIGHT channel (ADC_CHANNEL_%d) failed: %s",
                 (int)BSP_ADC_CH_LIGHT, esp_err_to_name(err));
        return err;
    }

    err = adc_oneshot_config_channel(s_unit, BSP_ADC_CH_RAIN, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config RAIN channel (ADC_CHANNEL_%d) failed: %s",
                 (int)BSP_ADC_CH_RAIN, esp_err_to_name(err));
        return err;
    }

    /* GPIO3（ADC1_CH2，strapping 脚，启动后作输入没问题）：预留扩展用 */
    err = adc_oneshot_config_channel(s_unit, ADC_CHANNEL_2, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config CH2 (GPIO3) failed: %s", esp_err_to_name(err));
    }

    /* GPIO10（ADC1_CH9）：外接【五位 AD 键盘】信号，ADKEY 驱动从这里采样。
     * ★ 注意 IO10 原本是 KEY1 数字按键，现已弃用（BSP_KEY_GPIO_KEY1=NC）。 */
    err = adc_oneshot_config_channel(s_unit, ADC_CHANNEL_9, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config CH9 (GPIO10) failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/** @brief 建曲线拟合校准句柄；失败不报错，只置 s_cali_valid = false */
static void adc_bus_setup_calibration(void)
{
    s_cali = NULL;
    s_cali_valid = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    /* S3 不做逐通道补偿，一份句柄覆盖两个通道；chan 只是配置项，
     * 这里填光敏通道，和官方 oneshot_read 例子写法一致 */
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BSP_ADC_UNIT,
        .chan = BSP_ADC_CH_LIGHT,
        .atten = BSP_ADC_ATTEN,
        .bitwidth = BSP_ADC_BITWIDTH,
    };

    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err == ESP_OK && s_cali != NULL) {
        s_cali_valid = true;
        ESP_LOGI(TAG, "calibration scheme: curve fitting");
    } else {
        s_cali = NULL;
        s_cali_valid = false;
        ESP_LOGW(TAG, "curve fitting calibration unavailable (%s), "
                      "fallback to linear approximation raw*%d/%d",
                 esp_err_to_name(err), ADC_LINEAR_FULL_SCALE_MV, ADC_RAW_FULL_SCALE);
    }
#else
    ESP_LOGW(TAG, "curve fitting scheme not built for this target, "
                  "fallback to linear approximation raw*%d/%d",
             ADC_LINEAR_FULL_SCALE_MV, ADC_RAW_FULL_SCALE);
#endif
}

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t adc_bus_init(void)
{
    /* 幂等 */
    if (s_inited) {
        return ESP_OK;
    }

    /* ★ 先建互斥锁：见文件头"为什么这里必须自己加一把锁" */
    if (s_adc_mutex == NULL) {
        s_adc_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_adc_mutex == NULL) {
            ESP_LOGE(TAG, "create adc mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = adc_bus_setup_unit_and_channels();
    if (err != ESP_OK) {
        return err;
    }

    adc_bus_setup_calibration();

    s_inited = true;
    /* ADC1 的 CHn 在 S3 上对应 GPIO(n+1)：CH0=GPIO1(光敏) / CH1=GPIO2(雨滴) */
    ESP_LOGI(TAG, "ADC bus ready (LIGHT=CH%d->GPIO%d / RAIN=CH%d->GPIO%d), 已加互斥锁防多任务抢锁",
             (int)BSP_ADC_CH_LIGHT, (int)BSP_ADC_CH_LIGHT + 1,
             (int)BSP_ADC_CH_RAIN, (int)BSP_ADC_CH_RAIN + 1);
    return ESP_OK;
}

esp_err_t adc_bus_read_raw(adc_channel_t ch, int *out_raw)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_unit == NULL) {
        ESP_LOGE(TAG, "read channel %d before adc_bus_init()", (int)ch);
        return ESP_ERR_INVALID_STATE;
    }

    /* 没有锁（极端：init 失败）也要能读，只是退化成旧行为 */
    if (s_adc_mutex != NULL && xSemaphoreTakeRecursive(s_adc_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "take adc mutex failed");
        return ESP_ERR_TIMEOUT;
    }

    /* 拿到我们自己的锁之后，IDF 内部那把 try-lock 就不该再被别人抢走；
     * 仍留几次重试，纯粹是兜底（例如驱动内部 timout 抖动）。 */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < ADC_READ_RETRY; i++) {
        *out_raw = 0;
        err = adc_oneshot_read(s_unit, ch, out_raw);
        if (err == ESP_OK) {
            break;
        }
        if (err != ESP_ERR_TIMEOUT) {
            break;      /* 非超时错误重试没意义 */
        }
    }

    /* 重试后仍失败：限速打一条带错误码的日志（以前只统计次数，不知道是 TIMEOUT） */
    if (err != ESP_OK) {
        static int64_t s_last_err_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_last_err_us > 5000000LL) {     /* 5 秒限速 */
            s_last_err_us = now;
            ESP_LOGW(TAG, "adc_oneshot_read(ch%d) 重试 %d 次仍失败: %s",
                     (int)ch, ADC_READ_RETRY, esp_err_to_name(err));
        }
    }

    if (s_adc_mutex != NULL) {
        xSemaphoreGiveRecursive(s_adc_mutex);
    }
    return err;
}

esp_err_t adc_bus_read_mv(adc_channel_t ch, int *out_mv)
{
    if (out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t err = adc_bus_read_raw(ch, &raw);
    if (err != ESP_OK) {
        /* 失败原因已经由 adc_bus_read_raw / adc_oneshot_read 打过日志，
         * 这里只把错误往上抛，避免 while 采样时刷屏 */
        return err;
    }

    if (s_cali_valid && s_cali != NULL) {
        err = adc_cali_raw_to_voltage(s_cali, raw, out_mv);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        /* 校准句柄在但转换失败：不吞错误，降级到线性近似并留日志 */
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed (%s), use linear approximation",
                 esp_err_to_name(err));
    }

    /* 线性近似兜底：mV = raw * 3100 / 4095 */
    *out_mv = (int)(((int32_t)raw * ADC_LINEAR_FULL_SCALE_MV) / ADC_RAW_FULL_SCALE);
    return ESP_OK;
}

int adc_bus_read_mv_avg(adc_channel_t ch, int samples)
{
    if (samples <= 0) {
        samples = ADC_AVG_SAMPLES_DEFAULT;
    } else if (samples > ADC_AVG_SAMPLES_MAX) {
        samples = ADC_AVG_SAMPLES_MAX;
    }

    int32_t sum_mv = 0;
    int ok_cnt = 0;

    for (int i = 0; i < samples; i++) {
        int mv = 0;
        if (adc_bus_read_mv(ch, &mv) == ESP_OK) {
            sum_mv += mv;
            ok_cnt++;
        }
        /* 单次失败只跳过，不打断整轮采样 */
    }

    if (ok_cnt == 0) {
        ESP_LOGE(TAG, "all %d samples failed on channel %d", samples, (int)ch);
        return -1;
    }

    /* 四舍五入 */
    return (int)((sum_mv + ok_cnt / 2) / ok_cnt);
}
