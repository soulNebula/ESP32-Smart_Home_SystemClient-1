/**
 * @file    sensor.c
 * @brief   传感器汇总实现：温湿度（SHT30/AHT20，I2C）+ 光照（BH1750 I2C / 光敏电阻 ADC）
 *          + 雨滴（ADC）
 *
 * 目标芯片：ESP32-S3-WROOM-1-N16R8    框架：ESP-IDF v5.4.x
 *
 * ===========================================================================
 *  【设计要点】
 *  1) 自动识别、插哪个用哪个：
 *       - 温湿度：探测 0x44(SHT30/SHT31) → 否则 0x38(AHT20) → 都没有则 valid_temp=false
 *       - 光照  ：探测 0x23(BH1750)     → 否则退回光敏电阻 ADC（百分比 + 等效 lux）
 *       - 雨滴  ：固定走 ADC（GPIO2 / ADC1_CH1）
 *     全部配件都不插时 sensor_init() 依然返回 ESP_OK，只打警告，绝不崩。
 *  2) I2C 走 BSP 自己的 i2c_bus（I2C/i2c_bus.c），ADC 走 adc_bus（ADC/adc_bus.c），
 *     本文件不新建总线、不新建 ADC 单元。
 *  3) 一阶低通滤波（alpha=0.30），每个量各自记录"是否已初始化"，首次采样直接赋值，
 *     不会从 0 慢慢爬；传感器掉线（valid_temp 变 false）或光照模式切换时会清掉
 *     "已初始化"标志，重新上线后直接跳到真实值，同样不会慢慢爬。
 *  4) sensor_get_last() 返回静态存储的只读指针，永不为 NULL。
 *  5) 采样过程中的单个传感器失败不会让 sensor_read() 失败：失败字段保留上次值，
 *     只有温湿度连续失败到阈值时才把 valid_temp 置 false（让上层联动自动停用）。
 *
 * ===========================================================================
 *  【接线约定与标定说明 —— 换配件后必须重看这里】
 *
 *  ▸ 光敏电阻（GPIO1 / ADC1_CH0）
 *      3V3 → 光敏电阻 → 中点(GPIO1) → 10kΩ → GND
 *      越亮 → 光敏阻值越小 → 中点电压越高（与 light_pct 同向）。
 *      折算：light_pct = clamp((mv - 100) * 100 / (3000 - 100), 0, 100)
 *            lux(等效) = light_pct^2 / 100 * 5      → 0~100% 映射到 0~500lux 的平方曲线，
 *                                                    暗区更敏感，便于用 BSP_DEF_LIGHT_* 阈值判断。
 *      ⚠ 换成 BH1750 之后 lux 就是真实 lux（量级完全不同，白天室内几百~上千 lux），
 *        BSP_DEF_LIGHT_ON_LUX / BSP_DEF_LIGHT_OFF_LUX 这两个阈值需要重新标定。
 *
 *  ▸ 雨滴模块（GPIO2 / ADC1_CH1）
 *      本工程约定：rain_pct 越大 = 越湿。默认按"越湿 → AO 电压越低"换算：
 *          rain_pct = clamp((3300 - rain_mv) * 100 / (3300 - 300), 0, 100)
 *      ⚠ 如果你的模块是"越湿电压越高"，把下面 rain_read() 里的
 *        (RAIN_MV_DRY - rain_mv) 改成 (rain_mv - RAIN_MV_WET) 即可（两个常数对调使用）。
 *      rain_detected 只是按默认阈值 BSP_DEF_RAIN_PCT 的初判，
 *      真正的联动阈值在 automation 模块里（可被 MQTT / 语音改）。
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

#include "driver/i2c_master.h"   /* 新版 I2C Master 驱动（非废弃的 driver/i2c.h） */

#include "adc_bus.h"
#include "i2c_bus.h"

static const char *TAG = "SENSOR";

/* ========================================================================= */
/*  可调参数                                                                 */
/* ========================================================================= */
#define SENSOR_FILTER_ALPHA         0.30f   /* 一阶低通系数：越小越平滑、越迟钝 */
#define SENSOR_ADC_SAMPLES          16      /* ADC 平均次数 */
#define SENSOR_DEF_PERIOD_MS        1000    /* 自动采样默认周期（BH1750 连续模式需 >=120ms） */
#define SENSOR_MIN_PERIOD_MS        100     /* 周期下限，防止任务把 CPU 吃满 */
#define SENSOR_REPROBE_PERIOD_MS    5000    /* "开机时没插上"的传感器每隔多久重新探测一次 */
#define SENSOR_FAIL_OFFLINE         3       /* 连续失败几次后判定传感器掉线 */
#define SENSOR_TASK_STACK           4096
#define SENSOR_TASK_PRIO            4
#define SENSOR_I2C_DEV_MAX          4       /* 缓存的 I2C 设备句柄上限（SHT30/AHT20/BH1750 + 余量） */

/* ---- 光敏电阻分压标定（对应上面的接线约定） ---- */
#define LIGHT_MV_DARK               100.0f  /* 认为"全黑"的电压 mV */
#define LIGHT_MV_BRIGHT             3000.0f /* 认为"全亮"的电压 mV（DB_12 量程约 3100mV，留裕量） */
#define LIGHT_EQ_LUX_GAIN           5.0f    /* 等效 lux = pct^2 / 100 * GAIN → 100% ↔ 500lux */
#define LIGHT_EQ_LUX_FULL           (100.0f * LIGHT_EQ_LUX_GAIN)

/* ---- 雨滴模块标定 ---- */
#define RAIN_MV_DRY                 3300.0f /* 完全干燥时的 AO 电压 mV */
#define RAIN_MV_WET                 300.0f  /* 完全浸湿时的 AO 电压 mV */

/* ---- 器件命令字 ---- */
#define SHT30_CMD_MEAS_HI           0x2C06  /* 单次测量、高重复性、无时钟拉伸 */
#define SHT30_MEAS_DELAY_MS         20
#define AHT20_CMD_INIT              0xBE    /* {0xBE,0x08,0x00} 初始化/校准 */
#define AHT20_CMD_MEAS              0xAC    /* {0xAC,0x33,0x00} 触发测量 */
#define AHT20_ARG_0                 0x00
#define AHT20_ARG_INIT              0x08
#define AHT20_ARG_MEAS              0x33
#define AHT20_INIT_DELAY_MS         10
#define AHT20_MEAS_DELAY_MS         80
#define AHT20_STATUS_BUSY           0x80    /* byte0 bit7 = 忙 */
#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_CONT_H_RES       0x10    /* 连续 H-Resolution 模式，周期约 120ms */
#define BH1750_POWER_ON_DELAY_MS    10

/* ========================================================================= */
/*  内部状态                                                                 */
/* ========================================================================= */
typedef enum {
    TH_CHIP_NONE = 0,
    TH_CHIP_SHT30,
    TH_CHIP_AHT20,
} th_chip_t;

typedef struct {
    uint8_t                  addr;
    i2c_master_dev_handle_t  handle;
} i2c_dev_cache_t;

static bool              s_inited;                       /* 初始化幂等标志 */
static sensor_data_t     s_data;                         /* 最近一次采样结果（静态存储，永不为 NULL） */

/* 只保护 s_data 的"整体结构体拷贝"，临界区只有几十字节，不申请堆、不会被 delete 拖死 */
static portMUX_TYPE      s_data_mux = portMUX_INITIALIZER_UNLOCKED;

static th_chip_t         s_th_chip      = TH_CHIP_NONE;
static bool              s_light_bh1750;                 /* true=BH1750 真实 lux；false=光敏电阻 ADC */
static int               s_temp_fail;                    /* 温湿度连续失败次数 */
static int               s_light_fail;                   /* BH1750 连续失败次数 */
static int64_t           s_last_probe_us;                /* 上次重新探测时刻 */

static bool              s_filt_temp_ready;              /* 滤波初值标志：首采直接赋值 */
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

/* ========================================================================= */
/*  小工具                                                                   */
/* ========================================================================= */
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

/**
 * @brief 一阶低通滤波：first = old + alpha*(raw - old)
 * @param old   上次滤波结果
 * @param raw   本次原始值
 * @param ready 该量是否已经采过一次；首次直接赋值，避免从 0 慢慢爬
 */
static float lowpass(float old, float raw, bool *ready)
{
    if (!*ready) {
        *ready = true;
        return raw;
    }
    return old + SENSOR_FILTER_ALPHA * (raw - old);
}

/**
 * @brief 毫秒延时：调度器已启动就用 vTaskDelay（让出 CPU），
 *        否则（上电极早期/调度器未启动）退回 ROM 忙等，保证任何上下文都能用。
 */
static void sensor_delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        esp_rom_delay_us(ms * 1000U);
    }
}

static void sensor_lock(void)
{
    portENTER_CRITICAL(&s_data_mux);
}

static void sensor_unlock(void)
{
    portEXIT_CRITICAL(&s_data_mux);
}

/**
 * @brief SHT30/AHT20 使用的 CRC-8：多项式 0x31、初值 0xFF、不取反
 */
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

/* ========================================================================= */
/*  I2C 收发：按地址缓存设备句柄，之后复用                                     */
/* ========================================================================= */

/**
 * @brief 取（必要时创建）某个从机地址的设备句柄
 * @note  i2c_master_bus_add_device() 返回 esp_err_t，句柄通过出参拿到；
 *        拿到之后才能用 i2c_master_transmit() / i2c_master_receive()。
 */
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
        return ESP_ERR_INVALID_STATE;   /* 总线还没初始化好 */
    }
    if (s_dev_cache_num >= SENSOR_I2C_DEV_MAX) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = dev_addr,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
        .scl_wait_us     = 0,           /* 0 = 用驱动默认值，能容忍从机时钟拉伸 */
        .flags = {
            .disable_ack_check = 0,     /* 保留 ACK 检查，器件掉线时能拿到错误 */
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

/** @brief 向从机写 len 字节（含寄存器/命令） */
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

/** @brief 从从机读 len 字节（不发寄存器地址，SHT30/BH1750/AHT20 都是这样读） */
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

/* ========================================================================= */
/*  SHT30 / SHT31                                                            */
/* ========================================================================= */
static esp_err_t sht30_read(float *out_temp, float *out_rh)
{
    /* 单次测量命令 0x2C06（高重复性，无时钟拉伸） */
    const uint8_t cmd[2] = {
        (uint8_t)(SHT30_CMD_MEAS_HI >> 8),
        (uint8_t)(SHT30_CMD_MEAS_HI & 0xFF),
    };
    esp_err_t err = i2c_write(BSP_I2C_ADDR_SHT30, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        return err;
    }

    sensor_delay_ms(SHT30_MEAS_DELAY_MS);   /* 高重复性约需 15ms，这里留 20ms */

    uint8_t buf[6] = {0};                   /* T_MSB T_LSB T_CRC RH_MSB RH_LSB RH_CRC */
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

    *out_temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;          /* 浮点除法，无整数截断 */
    *out_rh   = clampf(100.0f * (float)raw_rh / 65535.0f, 0.0f, 100.0f);
    return ESP_OK;
}

/* ========================================================================= */
/*  AHT20                                                                    */
/* ========================================================================= */

/** @brief 上电初始化/校准命令 {0xBE,0x08,0x00}；失败只警告，不阻止后续尝试 */
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

static esp_err_t aht20_read(float *out_temp, float *out_rh)
{
    const uint8_t cmd[3] = {AHT20_CMD_MEAS, AHT20_ARG_MEAS, AHT20_ARG_0};
    esp_err_t err = i2c_write(BSP_I2C_ADDR_AHT20, cmd, sizeof(cmd));
    if (err != ESP_OK) {
        return err;
    }

    sensor_delay_ms(AHT20_MEAS_DELAY_MS);   /* 测量约需 75ms，这里留 80ms */

    uint8_t buf[7] = {0};                   /* [0]=状态 [1..5]=数据 [6]=CRC */
    err = i2c_read(BSP_I2C_ADDR_AHT20, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    if ((buf[0] & AHT20_STATUS_BUSY) != 0) {
        /* bit7=1 表示芯片还在测。用 ESP_ERR_NOT_FINISHED 与"通信失败"区分开，
         * 上层不会把它算进掉线计数，下次采样再读即可。 */
        ESP_LOGW(TAG, "AHT20 忙标志未清（本次未测完），跳过本次采样");
        return ESP_ERR_NOT_FINISHED;
    }

    /* 数据手册 CRC：对前 6 字节做 CRC-8(0x31, init 0xFF)，与 buf[6] 比较 */
    if (crc8_sensirion(&buf[0], 6) != buf[6]) {
        ESP_LOGW(TAG, "AHT20 CRC 校验失败，丢弃本次数据（保留上次值）");
        return ESP_ERR_INVALID_CRC;
    }

    /* 20bit 湿度：buf[1..2] + buf[3] 高 4 位 */
    uint32_t raw_h = (((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4));
    /* 20bit 温度：buf[3] 低 4 位 + buf[4..5] */
    uint32_t raw_t = ((((uint32_t)buf[3] & 0x0FU) << 16) | ((uint32_t)buf[4] << 8) | (uint32_t)buf[5]);

    *out_rh   = clampf((float)raw_h * 100.0f / 1048576.0f, 0.0f, 100.0f);
    *out_temp = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
}

/* ========================================================================= */
/*  BH1750                                                                   */
/* ========================================================================= */
static esp_err_t bh1750_begin(void)
{
    uint8_t cmd = BH1750_CMD_POWER_ON;
    esp_err_t err = i2c_write(BSP_I2C_ADDR_BH1750, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    sensor_delay_ms(BH1750_POWER_ON_DELAY_MS);

    cmd = BH1750_CMD_CONT_H_RES;            /* 连续 H-Resolution：每 ~120ms 更新一次 */
    err = i2c_write(BSP_I2C_ADDR_BH1750, &cmd, 1);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t bh1750_read(float *out_lux)
{
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_read(BSP_I2C_ADDR_BH1750, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    uint16_t raw = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    *out_lux = (float)raw / 1.2f;           /* 数据手册：lux = raw / 1.2 */
    return ESP_OK;
}

/* ========================================================================= */
/*  ADC：光敏电阻 / 雨滴                                                      */
/* ========================================================================= */

/** @brief 光敏电阻 ADC 电压 → 0~100%（越大越亮）
 *
 *  ★ 极性：本模块实测是【反极性】（亮 → 电压低），由 board_config.h 的
 *    BSP_LIGHT_ADC_INVERT 决定是否翻转。搞反的表现是"关灯反而显示 100%"，
 *    进而让自动联动把灯关掉、窗帘拉开（2026-09-29 实机踩到）。 */
static float light_mv_to_pct(int mv)
{
    float pct = ((float)mv - LIGHT_MV_DARK) * 100.0f / (LIGHT_MV_BRIGHT - LIGHT_MV_DARK);
    pct = clampf(pct, 0.0f, 100.0f);
#if BSP_LIGHT_ADC_INVERT
    pct = 100.0f - pct;
#endif
    return pct;
}

/**
 * @brief 光敏电阻百分比 → 等效 lux（仅单调映射，供上层统一按 lux 处理）
 *
 *   lux = light_pct^2 / 100 * LIGHT_EQ_LUX_GAIN
 *       → 0% = 0lux，50% = 125lux，100% = 500lux。平方曲线让暗区更敏感，
 *         这样 BSP_DEF_LIGHT_ON_LUX(50) / BSP_DEF_LIGHT_OFF_LUX(200) 两个阈值
 *         正好落在"比较暗"和"比较亮"的位置。
 *  ⚠ 这【不是】真实 lux。换成 BH1750 后读到的是真实 lux，量级差很多，
 *    这两个阈值必须重新标定（见文件头说明）。
 */
static float light_pct_to_eq_lux(float pct)
{
    return pct * pct / 100.0f * LIGHT_EQ_LUX_GAIN;
}

/** @brief BH1750 真实 lux → 0~100%（按 500lux 饱和，仅供 OLED/UI 参考） */
static float lux_to_pct(float lux)
{
    return clampf(lux * 100.0f / LIGHT_EQ_LUX_FULL, 0.0f, 100.0f);
}

/**
 * @brief 雨滴 AO 电压 → 0~100%（越大越湿）
 * @note  默认按"越湿电压越低"：pct = (RAIN_MV_DRY - mv) / (RAIN_MV_DRY - RAIN_MV_WET) * 100
 *        若你的模块相反，把分子改成 (mv - RAIN_MV_WET)（即两个常数对调）。
 */
static float rain_mv_to_pct(int mv)
{
    float pct = (RAIN_MV_DRY - (float)mv) * 100.0f / (RAIN_MV_DRY - RAIN_MV_WET);
    return clampf(pct, 0.0f, 100.0f);
}

/* ========================================================================= */
/*  自动识别（含热插拔重探测）                                                */
/* ========================================================================= */
static void sensor_redetect(void)
{
    /* "温湿度没找到"的警告只打一次。
     * 本函数每 SENSOR_REPROBE_PERIOD_MS(5s) 跑一次，配件没到时会一直刷同一条警告，
     * 把串口淹掉；找到传感器后把标志清掉，下次掉了会再警告一次。 */
    static bool s_th_missing_warned = false;

    if (i2c_bus_get_handle() == NULL) {
        return;                     /* 总线没起来，探测没有意义 */
    }

    /* 温湿度：只有"从没识别到过"的芯片才重新探测。
     * 已经识别过、后来被拔掉的芯片走失败计数逻辑，不需要在这里反复 probe。 */
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
            (void)aht20_begin();    /* 失败只警告，读取时还会再试 */
        } else if (!s_th_missing_warned) {
            ESP_LOGW(TAG, "未检测到温湿度传感器（0x%02X / 0x%02X），温度联动将停用",
                     BSP_I2C_ADDR_SHT30, BSP_I2C_ADDR_AHT20);
            s_th_missing_warned = true;
        }
    }

    /* 光照：当前是光敏电阻模式时才去试探 BH1750 */
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

/* ========================================================================= */
/*  对外接口                                                                  */
/* ========================================================================= */
esp_err_t sensor_init(void)
{
    if (s_inited) {
        return ESP_OK;              /* 幂等：重复调用直接返回 */
    }
    s_inited = true;                /* 先置位，避免下面任何路径重入 */

    memset(&s_data, 0, sizeof(s_data));
    s_data.valid_temp = false;      /* 未识别到温湿度芯片前，"无意义" */
    s_data.light_is_bh1750 = false;
    s_last_probe_us = esp_timer_get_time();

    /* ---- 1. I2C 总线（幂等，复用 i2c_bus 模块的总线） ---- */
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_bus_init 失败: %s，温湿度/光照(BH1750) 将不可用", esp_err_to_name(err));
    }

    /* ---- 2. ADC（幂等，复用 adc_bus 模块的单元） ---- */
    err = adc_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_bus_init 失败: %s，光敏电阻/雨滴 将不可用", esp_err_to_name(err));
    }

    /* ---- 3. 自动识别配件 ---- */
    sensor_redetect();

    if (i2c_bus_get_handle() == NULL) {
        ESP_LOGW(TAG, "I2C 总线不可用，跳过地址扫描");
    } else if (s_th_chip == TH_CHIP_NONE && !s_light_bh1750) {
        /* 一个 I2C 传感器都没认出来时扫一遍总线，方便排查地址/接线问题 */
        (void)i2c_bus_scan();
    }

    ESP_LOGI(TAG, "初始化完成：温湿度=%s，光照=%s，雨滴=ADC(%d)",
             (s_th_chip == TH_CHIP_SHT30) ? "SHT30" :
             (s_th_chip == TH_CHIP_AHT20) ? "AHT20" : "未接(valid_temp=false)",
             s_light_bh1750 ? "BH1750(lux)" : "光敏电阻(ADC 等效 lux)",
             (int)BSP_ADC_CH_RAIN);

    /* 即使一个配件都没插，也返回 ESP_OK —— 上层照常跑，只是相关联动无效 */
    return ESP_OK;
}

esp_err_t sensor_read(sensor_data_t *out)
{
    if (!s_inited) {
        esp_err_t err = sensor_init();      /* 容错：没初始化也能直接采 */
        if (err != ESP_OK) {
            return err;
        }
    }

    /* 每隔一段时间重新探测"开机时没插上"的配件（热插拔友好） */
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_probe_us) >= ((int64_t)SENSOR_REPROBE_PERIOD_MS * 1000)) {
        s_last_probe_us = now_us;
        sensor_redetect();
    }

    /* 以"上次结果"为基准：本轮读失败的字段自然保留上次值 */
    sensor_data_t d;
    sensor_lock();
    d = s_data;
    sensor_unlock();

    /* ---------------- 1. 温湿度 ---------------- */
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
            /* CRC 错误：本次数据已丢弃（保留上次值）；NOT_FINISHED（AHT20 忙）：
             * 只是还没测完，两者都不算"掉线"，不计入失败次数。 */
            s_temp_fail++;
            if (s_temp_fail == SENSOR_FAIL_OFFLINE) {
                d.valid_temp = false;
                s_filt_temp_ready = false;      /* 恢复时直接跳回真实值，不从旧值慢慢爬 */
                s_filt_hum_ready = false;
                ESP_LOGW(TAG, "温湿度传感器连续 %d 次读取失败（%s），valid_temp=false",
                         s_temp_fail, esp_err_to_name(err));
            }
        }
    }

    /* ---------------- 2. 光照 ---------------- */
    /* 光敏电阻的 ADC 值两种模式都读（BH1750 模式下 light_mv 仅作参考） */
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
            d.light_pct = lux_to_pct(d.lux);        /* BH1750 模式下百分比由真实 lux 折算 */
        } else {
            s_light_fail++;
            if (s_light_fail >= SENSOR_FAIL_OFFLINE) {
                ESP_LOGW(TAG, "BH1750 连续 %d 次读取失败（%s），退回光敏电阻 ADC 模式",
                         s_light_fail, esp_err_to_name(err));
                s_light_bh1750 = false;
                s_light_fail = 0;
                s_filt_lux_ready = false;
                s_filt_pct_ready = false;           /* 换模式后重新取初值 */
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
            d.lux = light_pct_to_eq_lux(d.light_pct);   /* 见文件头：等效 lux，换 BH1750 后需重标定 */
        }
        d.light_is_bh1750 = false;
    }

    /* ---------------- 3. 雨滴 ---------------- */
    int rain_mv = adc_bus_read_mv_avg(BSP_ADC_CH_RAIN, SENSOR_ADC_SAMPLES);
    if (rain_mv >= 0) {
        d.rain_mv = rain_mv;
        d.rain_pct = lowpass(d.rain_pct, rain_mv_to_pct(rain_mv), &s_filt_rain_ready);
        /* 这里只是按默认阈值的初判，真正的联动阈值在 automation 模块 */
        d.rain_detected = (d.rain_pct > (float)BSP_DEF_RAIN_PCT);
    } else {
        ESP_LOGW(TAG, "雨滴 ADC 读取失败，rain_pct 保留上次值");
    }

    /* ---------------- 4. 回写缓存 / 输出 / 回调 ---------------- */
    d.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    sensor_lock();
    s_data = d;
    sensor_unlock();

    if (out != NULL) {
        *out = d;
    }

    /* 回调策略：每一轮采样按 kind 调用 3 次（TEMP / LIGHT / RAIN），
     * 数据指针指向内部缓存（sensor_get_last()），有效与否看 valid_temp /
     * light_is_bh1750 等标志位。回调在采样上下文执行，请不要在里面再次
     * 调用 sensor_read() 或做耗时操作。 */
    sensor_cb_t cb = s_cb;
    if (cb != NULL) {
        const sensor_data_t *last = sensor_get_last();
        cb(SENSOR_KIND_TEMP, last, s_cb_user);
        cb(SENSOR_KIND_LIGHT, last, s_cb_user);
        cb(SENSOR_KIND_RAIN, last, s_cb_user);
    }

    /* 单个传感器不在线/读失败不算整体失败：相关字段有 valid_* 标志或保留上次值 */
    return ESP_OK;
}

const sensor_data_t *sensor_get_last(void)
{
    return &s_data;     /* 静态存储，任何时刻都不为 NULL（未初始化时是全 0 + valid_temp=false） */
}

esp_err_t sensor_register_cb(sensor_cb_t cb, void *user_data)
{
    /* 传 NULL 表示注销回调 */
    s_cb = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

/* ========================================================================= */
/*  自动采样任务                                                              */
/* ========================================================================= */
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
        return ESP_OK;              /* 幂等 */
    }
    s_task = NULL;

    if (task == xTaskGetCurrentTaskHandle()) {
        /* 在采样任务自己里面调用：清掉句柄后自杀，避免 vTaskDelete 自己导致句柄悬空 */
        ESP_LOGW(TAG, "在采样任务内部调用 sensor_stop_auto()，任务将立即结束");
        vTaskDelete(NULL);
        return ESP_OK;              /* 到不了这里 */
    }

    vTaskDelete(task);
    ESP_LOGI(TAG, "自动采样任务已停止");
    return ESP_OK;
}

/* ========================================================================= */
/*  可读性辅助                                                                */
/* ========================================================================= */
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
        snprintf(t_str, sizeof(t_str), "T:--");     /* 温湿度芯片不在线 */
        snprintf(h_str, sizeof(h_str), "H:--");
    }

    if (d->light_is_bh1750) {
        snprintf(l_str, sizeof(l_str), "L:%.1flux", d->lux);    /* BH1750：真实 lux */
    } else {
        /* 光敏电阻：百分比 + 原始 mV —— 带上 mV 是为了现场一眼判定 AO 极性
         * （关灯应接近 0%、mV 低；见 board_config.h 的 BSP_LIGHT_ADC_INVERT） */
        snprintf(l_str, sizeof(l_str), "L:%.1f%%(%dmV)", d->light_pct, d->light_mv);
    }

    snprintf(r_str, sizeof(r_str), "Rain:%.1f%%", d->rain_pct);

    snprintf(buf, len, "%s %s %s %s", t_str, h_str, l_str, r_str);
}
