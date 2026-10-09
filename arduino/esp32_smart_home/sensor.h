#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ======================================================================
// 传感器：温湿度 AHT20 + 光敏电阻 + 雨滴
//
// 对应 ESP-IDF 工程的 components/BSP/SENSOR/sensor.c。
// 原工程用 FreeRTOS 任务定时采，Arduino 这份改成主循环喊 sensor_poll()，
// 到点才真去采，不占着 CPU 干等。
// ======================================================================

typedef struct {
    // 温度，单位摄氏度
    float temperature;
    // 湿度，按百分比算
    float humidity;
    // 温湿度芯片在不在
    bool  valid_temp;

    // 亮度百分比，越大越亮
    float light_pct;
    // 光敏电阻电压，毫伏
    int   light_mv;

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
bool sensor_init(void);

// 马上采一次
bool sensor_read(sensor_data_t *out);

// 取上次采样结果
const sensor_data_t *sensor_get_last(void);

// 登记采完的回调，只留最后一个
void sensor_register_cb(sensor_cb_t cb, void *user_data);

// 开自动采样，给周期毫秒
void sensor_start_auto(uint32_t period_ms);

// 停掉自动采样
void sensor_stop_auto(void);

// 主循环喊这个，到点才真采
void sensor_poll(void);

// 拼一行给人看的字
void sensor_format_line(const sensor_data_t *d, char *buf, size_t len);
