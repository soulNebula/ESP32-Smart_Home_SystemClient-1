/**
 * @file  sensor.h
 * @brief 传感器汇总：SHT30/AHT20（温湿度，I2C）+ 光照 + 雨滴
 *
 *  【自动识别，插哪个用哪个】
 *   温湿度：上电时探测 I2C 0x44(SHT30/SHT31) → 没有就试 0x38(AHT20)
 *          都没有 = valid_temp=false，温度相关联动自动停用，不报错。
 *   光照  ：上电时探测 I2C 0x23(BH1750) → 有就直接读 lux（无需标定）
 *          没有就退回 GPIO1 上的光敏电阻分压（ADC，用百分比表示）。
 *   雨滴  ：GPIO2 上的 AO 模拟量（ADC），换算成 0~100% 湿度；
 *          GPIO12 上的 DO 数字量可选，不用也能跑。
 *
 * 所有量都做了一阶低通滤波，避免数值跳动导致联动频繁触发。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <math.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* —— 温湿度 —— */
    float temperature;      /**< 摄氏度；valid_temp=false 时无意义 */
    float humidity;         /**< 相对湿度 %RH */
    bool  valid_temp;       /**< SHT30/AHT20 是否在线 */

    /* —— 光照 —— */
    float lux;              /**< 光照。BH1750 模式=真实 lux；光敏模式=折算的等效 lux */
    float light_pct;        /**< 0~100，越大越亮（两种模式都有） */
    int   light_mv;         /**< 光敏电阻分压电压 mV（BH1750 模式下也保留 ADC 值） */
    bool  light_is_bh1750;  /**< true=BH1750 真实 lux；false=光敏电阻折算 */

    /* —— 雨滴 —— */
    int   rain_mv;          /**< 雨滴模块 AO 电压 mV */
    float rain_pct;         /**< 0~100，越大越湿（下雨） */
    bool  rain_detected;    /**< 是否判定为"正在下雨" */

    uint32_t timestamp_ms;  /**< 采样时刻（esp_timer_get_time/1000） */
} sensor_data_t;

/** 采样周期 / 采集完成通知 */
typedef enum {
    SENSOR_KIND_TEMP = 0,
    SENSOR_KIND_LIGHT,
    SENSOR_KIND_RAIN,
} sensor_kind_t;

typedef void (*sensor_cb_t)(sensor_kind_t kind, const sensor_data_t *data, void *user_data);

/** @brief 初始化传感器（自动探测型号）。全部缺失也返回 ESP_OK，只打警告 */
esp_err_t sensor_init(void);

/** @brief 立刻采一次，结果写入 out（out 可为 NULL，则只更新内部缓存） */
esp_err_t sensor_read(sensor_data_t *out);

/** @brief 取最近一次采样结果的只读指针（不会为 NULL） */
const sensor_data_t *sensor_get_last(void);

/** @brief 注册数据就绪回调 */
esp_err_t sensor_register_cb(sensor_cb_t cb, void *user_data);

/**
 * @brief 启动后台自动采样任务
 * @param period_ms 采样周期，建议 1000~2000ms（SHT30 最快 1Hz 足够）
 */
esp_err_t sensor_start_auto(uint32_t period_ms);

/** @brief 停掉自动采样 */
esp_err_t sensor_stop_auto(void);

/** @brief 人类可读的一行摘要，用于 OLED / 日志 */
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
