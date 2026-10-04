/*
 * 模块：
 *   ADC 采集。读光敏电阻和雨滴两路的电压，被 sensor.c、adkey.c 调用；
 *   因为两个常驻任务都会来读，这里自己加了一把锁防抢，
 *   向下调 IDF 的采样和校准接口。
 *
 * 功能：
 *   把采样单元准备好
 *   读原始值
 *   读电压毫伏
 *   多读几次取平均
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"          /* 功能：算时间差限日志 */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"           /* 功能：电压换算接口 */
#include "esp_adc/adc_cali_scheme.h"    /* 功能：校准方式接口 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "adc_bus.h"    /* 功能：含引脚配置 */

static const char *TAG = "ADC_BUS";

/* 功能：加锁防两个任务抢 */
static SemaphoreHandle_t s_adc_mutex = NULL;   /* 功能：用可重入的锁 */
#define ADC_READ_RETRY          3              /* 功能：失败重试三次 */

/* 功能：12位满值 */
#define ADC_RAW_FULL_SCALE      4095

/* 功能：没校准就用这个估 */
#define ADC_LINEAR_FULL_SCALE_MV    3100

/* 功能：平均最多读这么多次 */
#define ADC_AVG_SAMPLES_MAX     256
#define ADC_AVG_SAMPLES_DEFAULT 16

/* 功能：单例状态 */
static adc_oneshot_unit_handle_t s_unit = NULL;     /* 功能：采样单元 */
static adc_cali_handle_t         s_cali = NULL;     /* 功能：校准把手，可能空 */
static bool s_cali_valid = false;                   /* 功能：校准能不能用 */
static bool s_inited = false;                       /* 功能：记住已初始化 */

/* 功能：建单元并配好通道 */
static esp_err_t adc_bus_setup_unit_and_channels(void)
{
    if (s_unit == NULL) {
        /* 功能：时钟源用默认 */
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = BSP_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };

        esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
        if (err != ESP_OK) {
            s_unit = NULL;
            /* 功能：被别人先占了 */
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

    /* 功能：留一路给扩展 */
    err = adc_oneshot_config_channel(s_unit, ADC_CHANNEL_2, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config CH2 (GPIO3) failed: %s", esp_err_to_name(err));
    }

    /* 功能：五位键盘走这路 */
    err = adc_oneshot_config_channel(s_unit, ADC_CHANNEL_9, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config CH9 (GPIO10) failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/* 功能：建校准，失败也不报错 */
static void adc_bus_setup_calibration(void)
{
    s_cali = NULL;
    s_cali_valid = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    /* 功能：一份就够两路用 */
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

/* 功能：把采样单元准备好 */
esp_err_t adc_bus_init(void)
{
    /* 功能：起过就返回 */
    if (s_inited) {
        return ESP_OK;
    }

    /* 功能：先把锁建好 */
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
    /* 功能：通道号对应脚号 */
    ESP_LOGI(TAG, "ADC bus ready (LIGHT=CH%d->GPIO%d / RAIN=CH%d->GPIO%d), 已加互斥锁防多任务抢锁",
             (int)BSP_ADC_CH_LIGHT, (int)BSP_ADC_CH_LIGHT + 1,
             (int)BSP_ADC_CH_RAIN, (int)BSP_ADC_CH_RAIN + 1);
    return ESP_OK;
}

/* 功能：读一路的原始值 */
esp_err_t adc_bus_read_raw(adc_channel_t ch, int *out_raw)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_unit == NULL) {
        ESP_LOGE(TAG, "read channel %d before adc_bus_init()", (int)ch);
        return ESP_ERR_INVALID_STATE;
    }

    /* 功能：没锁也得能读 */
    if (s_adc_mutex != NULL && xSemaphoreTakeRecursive(s_adc_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "take adc mutex failed");
        return ESP_ERR_TIMEOUT;
    }

    /* 功能：留几次重试兜底 */
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < ADC_READ_RETRY; i++) {
        *out_raw = 0;
        err = adc_oneshot_read(s_unit, ch, out_raw);
        if (err == ESP_OK) {
            break;
        }
        if (err != ESP_ERR_TIMEOUT) {
            break;      /* 功能：别的错不用重试 */
        }
    }

    /* 功能：还失败就限速报错 */
    if (err != ESP_OK) {
        static int64_t s_last_err_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_last_err_us > 5000000LL) {     /* 功能：五秒报一次 */
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

/* 功能：读一路的电压 */
esp_err_t adc_bus_read_mv(adc_channel_t ch, int *out_mv)
{
    if (out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    esp_err_t err = adc_bus_read_raw(ch, &raw);
    if (err != ESP_OK) {
        /* 功能：错已报过不重复 */
        return err;
    }

    if (s_cali_valid && s_cali != NULL) {
        err = adc_cali_raw_to_voltage(s_cali, raw, out_mv);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        /* 功能：转不了就换估算 */
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed (%s), use linear approximation",
                 esp_err_to_name(err));
    }

    /* 功能：按比例估电压 */
    *out_mv = (int)(((int32_t)raw * ADC_LINEAR_FULL_SCALE_MV) / ADC_RAW_FULL_SCALE);
    return ESP_OK;
}

/* 功能：多读几次取平均 */
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
        /* 功能：一次失败就跳过 */
    }

    if (ok_cnt == 0) {
        ESP_LOGE(TAG, "all %d samples failed on channel %d", samples, (int)ch);
        return -1;
    }

    /* 功能：四舍五入 */
    return (int)((sum_mv + ok_cnt / 2) / ok_cnt);
}
