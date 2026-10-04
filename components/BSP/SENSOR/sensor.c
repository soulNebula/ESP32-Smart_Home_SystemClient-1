/*
 * 模块：
 *   传感器汇总。管温度湿度、亮度、下雨三路数据，采完存进自己的缓存；
 *   被 board.c 开机初始化、被 main.c 起采样任务，数据给 automation.c 做联动，
 *   也给 OLED、astra_glue.cpp、mqtt_app.c 读；自己向下调 i2c_bus 读
 *   SHT30/AHT20/BH1750，调 adc_bus 读光敏电阻和雨滴。
 *
 * 功能：
 *   开机认一遍配件
 *   没插上的过会儿再认
 *   采一次温湿光雨
 *   数值做平滑防跳
 *   连续读不到就判掉线
 *   起后台定时采样任务
 */

#include "sensor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "driver/i2c_master.h"   /* 功能：用新版 I2C 驱动 */

#include "adc_bus.h"
#include "i2c_bus.h"

static const char *TAG = "SENSOR";

/* 功能：可调参数都放这里 */
#define SENSOR_FILTER_ALPHA         0.30f   /* 功能：平滑系数，越小越迟钝 */
#define SENSOR_ADC_SAMPLES          16      /* 功能：多采几次取平均 */
#define SENSOR_DEF_PERIOD_MS        1000    /* 功能：默认多久采一次 */
#define SENSOR_MIN_PERIOD_MS        100     /* 功能：最快也得隔这么久 */
#define SENSOR_REPROBE_PERIOD_MS    5000    /* 功能：隔多久再找一次配件 */
#define SENSOR_FAIL_OFFLINE         3       /* 功能：连错几次就算掉线 */
#define SENSOR_TASK_STACK           4096
#define SENSOR_TASK_PRIO            4
#define SENSOR_I2C_DEV_MAX          4       /* 功能：芯片句柄最多记几个 */

/* 功能：光敏电阻的标定值 */
#define LIGHT_MV_DARK               100.0f  /* 功能：全黑时的电压 */
#define LIGHT_MV_BRIGHT             3000.0f /* 功能：全亮时的电压 */
#define LIGHT_EQ_LUX_GAIN           5.0f    /* 功能：百分比换成亮度值 */
#define LIGHT_EQ_LUX_FULL           (100.0f * LIGHT_EQ_LUX_GAIN)

/* 功能：雨滴模块的标定值 */
#define RAIN_MV_DRY                 3300.0f /* 功能：全干时的电压 */
#define RAIN_MV_WET                 300.0f  /* 功能：全湿时的电压 */

/* 功能：芯片命令字 */
#define SHT30_CMD_MEAS_HI           0x2C06  /* 功能：叫它测一次 */
#define SHT30_MEAS_DELAY_MS         20
#define AHT20_CMD_INIT              0xBE    /* 功能：让它先自校准 */
#define AHT20_CMD_MEAS              0xAC    /* 功能：叫它测一次 */
#define AHT20_ARG_0                 0x00
#define AHT20_ARG_INIT              0x08
#define AHT20_ARG_MEAS              0x33
#define AHT20_INIT_DELAY_MS         10
#define AHT20_MEAS_DELAY_MS         80
#define AHT20_STATUS_BUSY           0x80    /* 功能：看它忙不忙 */
#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_CONT_H_RES       0x10    /* 功能：让它一直连续测 */
#define BH1750_POWER_ON_DELAY_MS    10

/* 功能：内部状态变量都在这 */
typedef enum {
    TH_CHIP_NONE = 0,
    TH_CHIP_SHT30,
    TH_CHIP_AHT20,
} th_chip_t;

typedef struct {
    uint8_t                  addr;
    i2c_master_dev_handle_t  handle;
} i2c_dev_cache_t;

static bool              s_inited;                       /* 功能：记下有没有开过机 */
static sensor_data_t     s_data;                         /* 功能：存最近一次采样结果 */

/* 功能：加锁防抢，只护一小段 */
static portMUX_TYPE      s_data_mux = portMUX_INITIALIZER_UNLOCKED;

static th_chip_t         s_th_chip      = TH_CHIP_NONE;
static bool              s_light_bh1750;                 /* 功能：现在用哪个光敏件 */
static int               s_temp_fail;                    /* 功能：温湿度连错几次了 */
static int               s_light_fail;                   /* 功能：BH1750 连错几次了 */
static int64_t           s_last_probe_us;                /* 功能：上次找配件的时间 */

static bool              s_filt_temp_ready;              /* 功能：头一次直接取真值 */
static bool              s_filt_hum_ready;
static bool              s_filt_lux_ready;
static bool              s_filt_pct_ready;
static bool              s_filt_rain_ready;

static sensor_cb_t       s_cb;
static void             *s_cb_user;

static TaskHandle_t      s_task;
static volatile uint32_t s_period_ms = SENSOR_DEF_PERIOD_MS;

static i2c_dev_cache_t   s_dev_cache[SENSOR_I2C_DEV_MAX];
static size_t            s_dev_cache_num;

/* 功能：几个小工具函数 */
/* 功能：把数值夹在上下限内 */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* 功能：慢速平滑，去掉跳动 */
static float lowpass(float old, float raw, bool *ready)
{
    if (!*ready) {
        *ready = true;
        return raw;
    }
    return old + SENSOR_FILTER_ALPHA * (raw - old);
}

/* 功能：按情况等一小会儿 */
static void sensor_delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        esp_rom_delay_us(ms * 1000U);
    }
}

/* 功能：加锁防抢 */
static void sensor_lock(void)
{
    portENTER_CRITICAL(&s_data_mux);
}

/* 功能：开锁 */
static void sensor_unlock(void)
{
    portEXIT_CRITICAL(&s_data_mux);
}

/* 功能：算校验值查错 */
static uint8_t crc8_sensirion(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* 功能：读写 I2C 芯片 */

/* 功能：按地址取芯片句柄 */
static esp_err_t i2c_dev_get(uint8_t dev_addr, i2c_master_dev_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_dev_cache_num; i++) {
        if (s_dev_cache[i].addr == dev_addr) {
            *out_handle = s_dev_cache[i].handle;
            return ESP_OK;
        }
    }

    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;   /* 功能：总线还没准备好 */
    }
    if (s_dev_cache_num >= SENSOR_I2C_DEV_MAX) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = dev_addr,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
        .scl_wait_us     = 0,           /* 功能：用默认值更稳 */
        .flags = {
            .disable_ack_check = 0,     /* 功能：芯片掉了能报错 */
        },
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) 失败: %s", dev_addr, esp_err_to_name(err));
        return err;
    }

    s_dev_cache[s_dev_cache_num].addr   = dev_addr;
    s_dev_cache[s_dev_cache_num].handle = handle;
    s_dev_cache_num++;
    *out_handle = handle;
    return ESP_OK;
}

/* 功能：给芯片写命令 */
static esp_err_t i2c_write(uint8_t dev_addr, const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_dev_get(dev_addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit(dev, buf, len, BSP_I2C_TIMEOUT_MS);
}

/* 功能：从芯片读数据 */
static esp_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_dev_get(dev_addr, &dev);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(dev, buf, len, BSP_I2C_TIMEOUT_MS);
}

/* 功能：读 SHT30 的温度湿度 */
static esp_err_t sht30_read(float *out_temp, float *out_rh)
{
    /* 功能：叫它测一次 */
    const uint8_t cmd[2] = {
        (uint8_t)(SHT30_CMD_MEAS_HI >> 8),
        (uint8_t)(SHT30_CMD_MEAS_HI & 0xFF),
    };
    esp_err_t err = i2c_write(BSP_I2C_ADDR_SHT30, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        return err;
    }

    sensor_delay_ms(SHT30_MEAS_DELAY_MS);   /* 功能：等它测完 */

    uint8_t buf[6] = {0};                   /* 功能：读回六个字节 */
    err = i2c_read(BSP_I2C_ADDR_SHT30, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    if (crc8_sensirion(&buf[0], 2) != buf[2] || crc8_sensirion(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "SHT30 CRC 校验失败，丢弃本次数据（保留上次值）");
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t  = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    uint16_t raw_rh = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    *out_temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;          /* 功能：按公式算出温度 */
    *out_rh   = clampf(100.0f * (float)raw_rh / 65535.0f, 0.0f, 100.0f);
    return ESP_OK;
}

/* 功能：让 AHT20 先校准 */
static esp_err_t aht20_begin(void)
{
    const uint8_t cmd[3] = {AHT20_CMD_INIT, AHT20_ARG_INIT, AHT20_ARG_0};
    esp_err_t err = i2c_write(BSP_I2C_ADDR_AHT20, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 初始化命令失败: %s", esp_err_to_name(err));
        return err;
    }
    sensor_delay_ms(AHT20_INIT_DELAY_MS);
    return ESP_OK;
}

/* 功能：读 AHT20 的温度湿度 */
static esp_err_t aht20_read(float *out_temp, float *out_rh)
{
    const uint8_t cmd[3] = {AHT20_CMD_MEAS, AHT20_ARG_MEAS, AHT20_ARG_0};
    esp_err_t err = i2c_write(BSP_I2C_ADDR_AHT20, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        return err;
    }

    sensor_delay_ms(AHT20_MEAS_DELAY_MS);   /* 功能：等它测完 */

    uint8_t buf[7] = {0};                   /* 功能：读回状态和数据 */
    err = i2c_read(BSP_I2C_ADDR_AHT20, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    if ((buf[0] & AHT20_STATUS_BUSY) != 0) {
        /* 功能：还在测就下次再来 */
        ESP_LOGW(TAG, "AHT20 忙标志未清（本次未测完），跳过本次采样");
        return ESP_ERR_NOT_FINISHED;
    }

    /* 功能：查数据对不对 */
    if (crc8_sensirion(&buf[0], 6) != buf[6]) {
        ESP_LOGW(TAG, "AHT20 CRC 校验失败，丢弃本次数据（保留上次值）");
        return ESP_ERR_INVALID_CRC;
    }

    /* 功能：拼出湿度原始值 */
    uint32_t raw_h = (((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4));
    /* 功能：拼出温度原始值 */
    uint32_t raw_t = ((((uint32_t)buf[3] & 0x0FU) << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5]);

    *out_rh   = clampf((float)raw_h * 100.0f / 1048576.0f, 0.0f, 100.0f);
    *out_temp = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
}

/* 功能：把 BH1750 打开 */
static esp_err_t bh1750_begin(void)
{
    uint8_t cmd = BH1750_CMD_POWER_ON;
    esp_err_t err = i2c_write(BSP_I2C_ADDR_BH1750, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    sensor_delay_ms(BH1750_POWER_ON_DELAY_MS);

    cmd = BH1750_CMD_CONT_H_RES;            /* 功能：让它一直连续测 */
    err = i2c_write(BSP_I2C_ADDR_BH1750, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

/* 功能：读 BH1750 的亮度 */
static esp_err_t bh1750_read(float *out_lux)
{
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_read(BSP_I2C_ADDR_BH1750, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    uint16_t raw = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    *out_lux = (float)raw / 1.2f;           /* 功能：按公式换成亮度 */
    return ESP_OK;
}

/* 功能：读光敏电阻的电压 */

/* 功能：电压换成亮度百分比 */
static float light_mv_to_pct(int mv)
{
    float pct = ((float)mv - LIGHT_MV_DARK) * 100.0f / (LIGHT_MV_BRIGHT - LIGHT_MV_DARK);
    pct = clampf(pct, 0.0f, 100.0f);
    /* 功能：接反了就翻过来 */
#if BSP_LIGHT_ADC_INVERT
    pct = 100.0f - pct;
#endif
    return pct;
}

/* 功能：百分比换等效亮度值 */
static float light_pct_to_eq_lux(float pct)
{
    return pct * pct / 100.0f * LIGHT_EQ_LUX_GAIN;
}

/* 功能：真实亮度换成百分比 */
static float lux_to_pct(float lux)
{
    return clampf(lux * 100.0f / LIGHT_EQ_LUX_FULL, 0.0f, 100.0f);
}

/* 功能：电压换成下雨百分比 */
static float rain_mv_to_pct(int mv)
{
    float pct = (RAIN_MV_DRY - (float)mv) * 100.0f / (RAIN_MV_DRY - RAIN_MV_WET);
    return clampf(pct, 0.0f, 100.0f);
}

/* 功能：自己找一遍配件 */
static void sensor_redetect(void)
{
    /* 功能：缺件警告只打一次 */
    static bool s_th_missing_warned = false;

    if (i2c_bus_get_handle() == NULL) {
        return;                     /* 功能：总线没好就不找 */
    }

    /* 功能：没认到过才重新找 */
    if (s_th_chip == TH_CHIP_NONE) {
        if (i2c_bus_probe(BSP_I2C_ADDR_SHT30) == ESP_OK) {
            s_th_chip = TH_CHIP_SHT30;
            s_temp_fail = 0;
            s_filt_temp_ready = false;
            s_filt_hum_ready = false;
            s_th_missing_warned = false;
            ESP_LOGI(TAG, "检测到温湿度传感器：SHT30/SHT31 (0x%02X)", BSP_I2C_ADDR_SHT30);
        } else if (i2c_bus_probe(BSP_I2C_ADDR_AHT20) == ESP_OK) {
            s_th_chip = TH_CHIP_AHT20;
            s_temp_fail = 0;
            s_filt_temp_ready = false;
            s_filt_hum_ready = false;
            s_th_missing_warned = false;
            ESP_LOGI(TAG, "检测到温湿度传感器：AHT20 (0x%02X)", BSP_I2C_ADDR_AHT20);
            (void)aht20_begin();    /* 功能：失败就下次再试 */
        } else if (!s_th_missing_warned) {
            ESP_LOGW(TAG, "未检测到温湿度传感器（0x%02X / 0x%02X），温度联动将停用",
                     BSP_I2C_ADDR_SHT30, BSP_I2C_ADDR_AHT20);
            s_th_missing_warned = true;
        }
    }

    /* 功能：还用电组才去找它 */
    if (!s_light_bh1750) {
        if (i2c_bus_probe(BSP_I2C_ADDR_BH1750) == ESP_OK) {
            if (bh1750_begin() == ESP_OK) {
                s_light_bh1750 = true;
                s_light_fail = 0;
                s_filt_lux_ready = false;
                ESP_LOGI(TAG, "检测到 BH1750 (0x%02X)，光照改用真实 lux", BSP_I2C_ADDR_BH1750);
            } else {
                ESP_LOGW(TAG, "BH1750 在线但初始化命令失败，继续用光敏电阻 ADC");
            }
        }
    }
}

/* 功能：开机把传感器备好 */
esp_err_t sensor_init(void)
{
    if (s_inited) {
        return ESP_OK;              /* 功能：开过就直接返回 */
    }
    s_inited = true;                /* 功能：先占位防重入 */

    memset(&s_data, 0, sizeof(s_data));
    s_data.valid_temp = false;      /* 功能：没认到芯片就无效 */
    s_data.light_is_bh1750 = false;
    s_last_probe_us = esp_timer_get_time();

    /* 功能：把 I2C 总线备好 */
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_bus_init 失败: %s，温湿度/光照(BH1750) 将不可用", esp_err_to_name(err));
    }

    /* 功能：把 ADC 单元备好 */
    err = adc_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_bus_init 失败: %s，光敏电阻/雨滴 将不可用", esp_err_to_name(err));
    }

    /* 功能：认一遍配件 */
    sensor_redetect();

    if (i2c_bus_get_handle() == NULL) {
        ESP_LOGW(TAG, "I2C 总线不可用，跳过地址扫描");
    } else if (s_th_chip == TH_CHIP_NONE && !s_light_bh1750) {
        /* 功能：都没认到就扫一遍 */
        (void)i2c_bus_scan();
    }

    ESP_LOGI(TAG, "初始化完成：温湿度=%s，光照=%s，雨滴=ADC(%d)",
             (s_th_chip == TH_CHIP_SHT30) ? "SHT30" :
             (s_th_chip == TH_CHIP_AHT20) ? "AHT20" : "未接(valid_temp=false)",
             s_light_bh1750 ? "BH1750(lux)" : "光敏电阻(ADC 等效 lux)",
             (int)BSP_ADC_CH_RAIN);

    /* 功能：缺件也不报错 */
    return ESP_OK;
}

esp_err_t sensor_read(sensor_data_t *out)
{
    if (!s_inited) {
        esp_err_t err = sensor_init();      /* 功能：没开过机就先开机 */
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 功能：过一阵再找一次配件 */
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_probe_us) >= ((int64_t)SENSOR_REPROBE_PERIOD_MS * 1000)) {
        s_last_probe_us = now_us;
        sensor_redetect();
    }

    /* 功能：拿上次结果打底 */
    sensor_data_t d;
    sensor_lock();
    d = s_data;
    sensor_unlock();

    /* 功能：读温度湿度 */
    if (s_th_chip != TH_CHIP_NONE) {
        float t = 0.0f;
        float rh = 0.0f;
        esp_err_t err = (s_th_chip == TH_CHIP_SHT30) ? sht30_read(&t, &rh) : aht20_read(&t, &rh);

        if (err == ESP_OK) {
            if (s_temp_fail >= SENSOR_FAIL_OFFLINE) {
                ESP_LOGI(TAG, "温湿度传感器已恢复");
            }
            s_temp_fail = 0;
            d.valid_temp = true;
            d.temperature = lowpass(d.temperature, t, &s_filt_temp_ready);
            d.humidity    = lowpass(d.humidity, rh, &s_filt_hum_ready);
        } else if (err != ESP_ERR_INVALID_CRC && err != ESP_ERR_NOT_FINISHED) {
            /* 功能：这两种错不算掉线 */
            s_temp_fail++;
            if (s_temp_fail == SENSOR_FAIL_OFFLINE) {
                d.valid_temp = false;
                s_filt_temp_ready = false;      /* 功能：好了就跳回真值 */
                s_filt_hum_ready = false;
                ESP_LOGW(TAG, "温湿度传感器连续 %d 次读取失败（%s），valid_temp=false",
                         s_temp_fail, esp_err_to_name(err));
            }
        }
    }

    /* 功能：读光照 */
    /* 功能：电压两种模式都读 */
    int light_mv = adc_bus_read_mv_avg(BSP_ADC_CH_LIGHT, SENSOR_ADC_SAMPLES);
    if (light_mv >= 0) {
        d.light_mv = light_mv;
    } else {
        ESP_LOGW(TAG, "光照 ADC 读取失败，light_mv 保留上次值");
    }

    if (s_light_bh1750) {
        float lux_raw = 0.0f;
        esp_err_t err = bh1750_read(&lux_raw);
        if (err == ESP_OK) {
            s_light_fail = 0;
            if (lux_raw < 0.0f) {
                lux_raw = 0.0f;
            }
            d.light_is_bh1750 = true;
            d.lux = lowpass(d.lux, lux_raw, &s_filt_lux_ready);
            d.light_pct = lux_to_pct(d.lux);        /* 功能：拿真实亮度算百分比 */
        } else {
            s_light_fail++;
            if (s_light_fail >= SENSOR_FAIL_OFFLINE) {
                ESP_LOGW(TAG, "BH1750 连续 %d 次读取失败（%s），退回光敏电阻 ADC 模式",
                         s_light_fail, esp_err_to_name(err));
                s_light_bh1750 = false;
                s_light_fail = 0;
                s_filt_lux_ready = false;
                s_filt_pct_ready = false;           /* 功能：换法子要重新取初值 */
                d.light_is_bh1750 = false;
            } else {
                ESP_LOGW(TAG, "BH1750 读取失败: %s（保留上次 lux）", esp_err_to_name(err));
            }
        }
    }

    if (!s_light_bh1750) {
        if (light_mv >= 0) {
            float pct = light_mv_to_pct(light_mv);
            d.light_pct = lowpass(d.light_pct, pct, &s_filt_pct_ready);
            d.lux = light_pct_to_eq_lux(d.light_pct);   /* 功能：百分比折成亮度值 */
        }
        d.light_is_bh1750 = false;
    }

    /* 功能：读下雨 */
    int rain_mv = adc_bus_read_mv_avg(BSP_ADC_CH_RAIN, SENSOR_ADC_SAMPLES);
    if (rain_mv >= 0) {
        d.rain_mv = rain_mv;
        d.rain_pct = lowpass(d.rain_pct, rain_mv_to_pct(rain_mv), &s_filt_rain_ready);
        /* 功能：先按默认值初判 */
        d.rain_detected = (d.rain_pct > (float)BSP_DEF_RAIN_PCT);
    } else {
        ESP_LOGW(TAG, "雨滴 ADC 读取失败，rain_pct 保留上次值");
    }

    /* 功能：存结果并通知上层 */
    d.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    sensor_lock();
    s_data = d;
    sensor_unlock();

    if (out != NULL) {
        *out = d;
    }

    /* 功能：每轮按三类各叫一次 */
    sensor_cb_t cb = s_cb;
    if (cb != NULL) {
        const sensor_data_t *last = sensor_get_last();
        cb(SENSOR_KIND_TEMP, last, s_cb_user);
        cb(SENSOR_KIND_LIGHT, last, s_cb_user);
        cb(SENSOR_KIND_RAIN, last, s_cb_user);
    }

    /* 功能：缺一个不算整体失败 */
    return ESP_OK;
}

const sensor_data_t *sensor_get_last(void)
{
    return &s_data;     /* 功能：这里的数据永远在 */
}

esp_err_t sensor_register_cb(sensor_cb_t cb, void *user_data)
{
    /* 功能：传空就是取消登记 */
    s_cb = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

/* 功能：定时采样的常驻任务 */
static void sensor_auto_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "自动采样任务启动，周期 %u ms", (unsigned)s_period_ms);

    while (1) {
        (void)sensor_read(NULL);
        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }
}

esp_err_t sensor_start_auto(uint32_t period_ms)
{
    if (period_ms == 0) {
        period_ms = SENSOR_DEF_PERIOD_MS;
    }
    if (period_ms < SENSOR_MIN_PERIOD_MS) {
        ESP_LOGW(TAG, "采样周期 %u ms 太小，钳到 %d ms", (unsigned)period_ms, SENSOR_MIN_PERIOD_MS);
        period_ms = SENSOR_MIN_PERIOD_MS;
    }
    s_period_ms = period_ms;

    if (s_task != NULL) {
        ESP_LOGI(TAG, "自动采样任务已在运行，周期更新为 %u ms", (unsigned)period_ms);
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(sensor_auto_task, "sensor_auto", SENSOR_TASK_STACK,
                                NULL, SENSOR_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "创建自动采样任务失败（内存不足？）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sensor_stop_auto(void)
{
    TaskHandle_t task = s_task;
    if (task == NULL) {
        return ESP_OK;              /* 功能：没在跑就返回 */
    }
    s_task = NULL;

    if (task == xTaskGetCurrentTaskHandle()) {
        /* 功能：自己停自己就先自杀 */
        ESP_LOGW(TAG, "在采样任务内部调用 sensor_stop_auto()，任务将立即结束");
        vTaskDelete(NULL);
        return ESP_OK;              /* 功能：走不到这一行 */
    }

    vTaskDelete(task);
    ESP_LOGI(TAG, "自动采样任务已停止");
    return ESP_OK;
}

/* 功能：拼一行给人看的字 */
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (d == NULL) {
        d = sensor_get_last();
    }

    char t_str[20];
    char h_str[20];
    char l_str[24];
    char r_str[24];

    if (d->valid_temp) {
        snprintf(t_str, sizeof(t_str), "T:%.1fC", d->temperature);
        snprintf(h_str, sizeof(h_str), "H:%.1f%%", d->humidity);
    } else {
        snprintf(t_str, sizeof(t_str), "T:--");     /* 功能：没芯片就打横杠 */
        snprintf(h_str, sizeof(h_str), "H:--");
    }

    if (d->light_is_bh1750) {
        snprintf(l_str, sizeof(l_str), "L:%.1flux", d->lux);    /* 功能：真实亮度 */
    } else {
        /* 功能：带上电压好查极性 */
        snprintf(l_str, sizeof(l_str), "L:%.1f%%(%dmV)", d->light_pct, d->light_mv);
    }

    snprintf(r_str, sizeof(r_str), "Rain:%.1f%%", d->rain_pct);

    snprintf(buf, len, "%s %s %s %s", t_str, h_str, l_str, r_str);
}
