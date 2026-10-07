#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <math.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 温度，单位摄氏度
    float temperature;
    // 湿度，按百分比算
    float humidity;
    // 温湿度芯片在不在
    bool  valid_temp;

    // 亮度，BH1750 时是真的
    float lux;
    // 亮度百分比，越大越亮
    float light_pct;
    // 光敏电阻电压，毫伏
    int   light_mv;
    // 是不是用的 BH1750
    bool  light_is_bh1750;

    // 雨滴电压，毫伏
    int   rain_mv;
    // 湿度百分比，越大越湿
    float rain_pct;
    // 是不是正在下雨
    bool  rain_detected;

    // 采样时刻，毫秒
    uint32_t timestamp_ms;
} sensor_data_t;

// 采完通知按哪类分
typedef enum {
    SENSOR_KIND_TEMP = 0,
    SENSOR_KIND_LIGHT,
    SENSOR_KIND_RAIN,
} sensor_kind_t;

// 采完就叫一下上层
typedef void (*sensor_cb_t)(sensor_kind_t kind, const sensor_data_t *data, void *user_data);

// 把硬件认一遍
esp_err_t sensor_init(void);

// 马上采一次
esp_err_t sensor_read(sensor_data_t *out);

// 取上次采样结果
const sensor_data_t *sensor_get_last(void);

// 登记采完的回调
esp_err_t sensor_register_cb(sensor_cb_t cb, void *user_data);

// 开后台定时采样
esp_err_t sensor_start_auto(uint32_t period_ms);

// 停掉定时采样
esp_err_t sensor_stop_auto(void);

// 拼一行给人看的字
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
