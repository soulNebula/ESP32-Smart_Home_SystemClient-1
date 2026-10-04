/*
 * 模块：
 *   传感器汇总。把温度、湿度、亮度、下雨这几样采回来，数据给 automation.c
 *   做联动，也给 OLED 和界面读；被 board.c 开机初始化、被 main.c 起任务，
 *   自己向下调 i2c_bus 读芯片、调 adc_bus 读电压。
 *
 * 功能：
 *   认一遍接了哪些配件
 *   采一次温湿光雨
 *   取最近一次结果
 *   登记采完的回调
 *   开后台定时采样
 *   停掉定时采样
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
    float temperature;      /* 功能：温度，单位摄氏度 */
    float humidity;         /* 功能：湿度，按百分比算 */
    bool  valid_temp;       /* 功能：温湿度芯片在不在 */

    float lux;              /* 功能：亮度，BH1750 时是真的 */
    float light_pct;        /* 功能：亮度百分比，越大越亮 */
    int   light_mv;         /* 功能：光敏电阻电压，毫伏 */
    bool  light_is_bh1750;  /* 功能：是不是用的 BH1750 */

    int   rain_mv;          /* 功能：雨滴电压，毫伏 */
    float rain_pct;         /* 功能：湿度百分比，越大越湿 */
    bool  rain_detected;    /* 功能：是不是正在下雨 */

    uint32_t timestamp_ms;  /* 功能：采样时刻，毫秒 */
} sensor_data_t;

/* 功能：采完通知按哪类分 */
typedef enum {
    SENSOR_KIND_TEMP = 0,
    SENSOR_KIND_LIGHT,
    SENSOR_KIND_RAIN,
} sensor_kind_t;

/* 功能：采完就叫一下上层 */
typedef void (*sensor_cb_t)(sensor_kind_t kind, const sensor_data_t *data, void *user_data);

/* 功能：把硬件认一遍 */
esp_err_t sensor_init(void);

/* 功能：马上采一次 */
esp_err_t sensor_read(sensor_data_t *out);

/* 功能：取上次采样结果 */
const sensor_data_t *sensor_get_last(void);

/* 功能：登记采完的回调 */
esp_err_t sensor_register_cb(sensor_cb_t cb, void *user_data);

/* 功能：开后台定时采样 */
esp_err_t sensor_start_auto(uint32_t period_ms);

/* 功能：停掉定时采样 */
esp_err_t sensor_stop_auto(void);

/* 功能：拼一行给人看的字 */
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
