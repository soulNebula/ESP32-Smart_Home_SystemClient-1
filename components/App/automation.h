/**
 * @file  automation.h
 * @brief 自动联动规则引擎 —— 对应需求 1 / 2 / 3
 *
 *   规则1（光照）：光照 < light_on_lux  且 自动模式开 → 开客厅灯 + 拉上窗帘（遮光）
 *                  光照 > light_off_lux 且 自动模式开 → 关客厅灯 + 拉开窗帘
 *                  ★ 方向和直觉一致：天黑了才需要开灯、才需要把窗帘拉上。
 *   规则2（温度）：温度 > temp_fan_on  → 开风扇（默认 70% 转速）
 *                  温度 < temp_fan_off → 关风扇
 *   规则3（雨滴）：雨滴湿度 > rain_pct  → 关窗
 *                  雨滴湿度回落            → 开窗（可关掉这条，见 cfg.auto_window_reopen）
 *
 * 【迟滞（hysteresis）】开/关用两个不同阈值，避免在阈值附近反复横跳。
 *   例如温度：28°C 开风扇，但要降到 26°C 才关 —— 中间这段不会来回切。
 *
 * 【手动优先】自动规则不会覆盖用户 3 秒内的手动操作（否则你刚用手机开灯，
 *   自动规则立刻又给关掉了）。见 AUTO_MANUAL_GUARD_MS。
 *
 * 阈值都能通过 MQTT 的 config topic 在运行时修改，不需要重新烧录。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "board_config.h"
#include "sensor.h"
#include "device_model.h"   /* 需要 device_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 阈值配置 */
typedef struct {
    bool  enabled;             /**< 自动模式总开关 */
    float light_on_lux;        /**< 低于此光照 → 开灯/拉帘（默认 BSP_DEF_LIGHT_ON_LUX） */
    float light_off_lux;       /**< 高于此光照 → 关灯/开帘（默认 BSP_DEF_LIGHT_OFF_LUX） */
    float temp_fan_on_c;       /**< 高于此温度 → 开风扇 */
    float temp_fan_off_c;      /**< 低于此温度 → 关风扇 */
    uint8_t fan_auto_speed;    /**< 自动开风扇时的转速%，默认 70 */
    float rain_pct;            /**< 高于此湿度 → 判下雨，关窗 */
    bool  auto_window_reopen;  /**< 雨停后是否自动重开窗，默认 false（安全起见） */
    bool  auto_light_enable;   /**< 是否启用光照联动 */
    bool  auto_temp_enable;    /**< 是否启用温度联动 */
    bool  auto_rain_enable;    /**< 是否启用雨滴联动 */
} automation_cfg_t;

/** @brief 初始化（载入 NVS 里保存的阈值，没有就用默认值） */
esp_err_t automation_init(void);

esp_err_t automation_set_enabled(bool enabled);
bool      automation_is_enabled(void);

/** @brief 取配置指针（可直接改，改完调用 automation_save() 落盘） */
automation_cfg_t *automation_get_cfg(void);

/** @brief 把当前配置存到 NVS，掉电不丢 */
esp_err_t automation_save(void);

/** @brief 从 NVS 载入配置 */
esp_err_t automation_load(void);

/**
 * @brief 按字符串键改单个阈值（MQTT config topic 用）
 * @param key   "light_on_lux" / "light_off_lux" / "temp_fan_on_c" /
 *              "temp_fan_off_c" / "rain_pct" / "fan_auto_speed" /
 *              "enabled" / "auto_light_enable" / "auto_temp_enable" /
 *              "auto_rain_enable" / "auto_window_reopen"
 * @return ESP_OK / ESP_ERR_INVALID_ARG（键不认识）/ ESP_ERR_INVALID_SIZE（值越界）
 */
esp_err_t automation_set_threshold(const char *key, float value);

/** @brief 把配置序列化成 JSON */
int automation_cfg_json(char *buf, size_t len);

/**
 * @brief 主循环周期性调用（建议 500ms~1s 一次），传入最新传感器数据
 *
 * 内部会做迟滞判断 + 手动操作保护期检查，然后调用 device_model 的 set 接口。
 */
void automation_tick(const sensor_data_t *d);

/**
 * @brief 告知自动化模块"刚刚有人手动操作了某设备"
 *
 * device_model 在 SRC_MQTT / SRC_VOICE / SRC_LOCAL_KEY 时会自动调用，
 * 用来启动保护期，避免规则立刻把用户的操作覆盖掉。
 */
void automation_notify_manual(device_id_t id);

/**
 * @brief 手动保护期时长（毫秒）
 *
 * 2026-09-29 由 3000 提到 60000：实测 3 秒太短 —— 用户在 App 里开灯，
 * 3 秒后规则就把它覆盖了（"打开过一会自己关了"），体感像是程序在跟人抢。
 * 现在手动动过的设备在 1 分钟内不受自动联动影响；之后规则照常接管。
 * 想让自动联动更快接管就把它调小（例如 10000）。
 */
#define AUTO_MANUAL_GUARD_MS  60000

#ifdef __cplusplus
}
#endif
