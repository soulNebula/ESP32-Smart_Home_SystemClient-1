#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "sensor.h"
// 要用设备编号
#include "device_model.h"

// ======================================================================
// 自动联动：光照、温度、下雨三条线
//
// 对应 ESP-IDF 工程的 components/App/automation.c。
// 阈值存在 NVS 里（Arduino 用 Preferences），掉电不丢。
// ======================================================================

// 联动用的那几条线
typedef struct {
    // 自动总开关
    bool  enabled;
    // 暗过这条就开灯
    float light_on_lux;
    // 亮过这条就关灯
    float light_off_lux;
    // 热过这条开风扇
    float temp_fan_on_c;
    // 凉过这条关风扇
    float temp_fan_off_c;
    // 自动开风扇的转速
    uint8_t fan_auto_speed;
    // 湿过这条当在下雨
    float rain_pct;
    // 雨停要不要自动开窗
    bool  auto_window_reopen;
    // 光照这条要不要用
    bool  auto_light_enable;
    // 温度这条要不要用
    bool  auto_temp_enable;
    // 雨滴这条要不要用
    bool  auto_rain_enable;
} automation_cfg_t;

// 手动之后歇多久不碰，单位毫秒
#define AUTO_MANUAL_GUARD_MS  60000

// 雨停重开的迟滞带：关窗按 rain_pct 阈值，重开要再干这么多才动
// 免得小雨在阈值附近反复开关窗
#define AUTO_RAIN_REOPEN_HYST_PCT  10.0f

// 上电读回存的那几条线
bool automation_init(void);

bool automation_set_enabled(bool enabled);
bool automation_is_enabled(void);

// 拿配置去改，改完记得存
automation_cfg_t *automation_get_cfg(void);

// 存起来，掉电不丢
bool automation_save(void);

// 从存的地方读回来
bool automation_load(void);

// 按名字改一条线
bool automation_set_threshold(const char *key, float value);

// 几条线拼成 JSON
int automation_cfg_json(char *buf, size_t len);

// 主循环隔会儿喊一次
void automation_tick(const sensor_data_t *d);

// 告诉联动刚有人动手
void automation_notify_manual(device_id_t id);
