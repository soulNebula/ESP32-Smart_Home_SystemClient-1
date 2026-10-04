/**
 * @file  device_model.h
 * @brief 设备模型 —— 全屋设备的统一状态机（App 层核心）
 *
 * 所有控制入口（语音 / 按键 / MQTT / 自动联动）都【只】调用本模块的 set 接口，
 * 由本模块负责：
 *   1) 更新软件状态
 *   2) 调用 BSP 驱动真正操作硬件
 *   3) 通过回调通知 MQTT（上报状态）/ OLED（刷新界面）
 *
 * 这样「谁改的」被记录下来（ctrl_source_t），方便日志排查和防抖。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 全屋设备清单 */
typedef enum {
    DEV_LED_LIVING = 0,  /**< 客厅灯带 */
    DEV_LED_KITCHEN,     /**< 厨房灯带 */
    DEV_LED_BEDROOM,     /**< 卧室灯带 */
    DEV_LED_BATH,        /**< 浴室灯带 */
    DEV_FAN,             /**< 风扇（0~100% 调速） */
    DEV_WINDOW,          /**< 窗户舵机（position 0=关 100=开） */
    DEV_DOOR,            /**< 门舵机 */
    DEV_CURTAIN,         /**< 窗帘舵机 */
    DEV_COUNT,
} device_id_t;

/** 控制来源 —— 用于日志、防抖、以及"自动模式下手动操作优先"策略 */
typedef enum {
    SRC_BOOT = 0,   /**< 上电初始化 */
    SRC_LOCAL_KEY,  /**< 板载按键 */
    SRC_VOICE,      /**< 语音 */
    SRC_MQTT,       /**< 手机 App（MQTT）/ 上位机 */
    SRC_BLE,        /**< 手机 App（BLE 蓝牙） */
    SRC_AUTO,       /**< 自动联动规则 */
    SRC_SELFTEST,   /**< 按键自检模式 */
} ctrl_source_t;

/** 单设备状态 */
typedef struct {
    bool     power;     /**< 开/关。舵机类：position>=50 视为"开" */
    uint8_t  level;     /**< 0~100：LED 亮度 / 风扇转速 / 舵机位置 */
    uint8_t  r, g, b;   /**< LED 颜色（非灯类无意义） */
    uint32_t change_cnt;/**< 状态变化次数，调试用 */
} device_state_t;

/** 状态变化回调：id 变了，src 是来源 */
typedef void (*device_event_cb_t)(device_id_t id, ctrl_source_t src, void *user_data);

/** @brief 初始化设备模型并把硬件置为默认状态（全关）。幂等 */
esp_err_t device_model_init(void);

/* ---------------- 控制接口 ---------------- */

/**
 * @brief 开 / 关
 * @note 对 DEV_FAN：on = 100% 转速；对舵机类：on = 转到 100%（全开）
 */
esp_err_t device_set_power(device_id_t id, bool on, ctrl_source_t src);

/** @brief 翻转当前状态 */
esp_err_t device_toggle(device_id_t id, ctrl_source_t src);

/**
 * @brief 设 0~100 的档位
 * @note LED=亮度，FAN=转速，WINDOW/DOOR/CURTAIN=开合位置（>=50 视为开）
 */
esp_err_t device_set_level(device_id_t id, uint8_t percent, ctrl_source_t src);

/** @brief 设灯带颜色（仅 DEV_LED_* 有效） */
esp_err_t device_set_color(device_id_t id, uint8_t r, uint8_t g, uint8_t b, ctrl_source_t src);

/** @brief 全部关闭（灯/风扇），舵机归位（关窗关门合帘） */
esp_err_t device_all_off(ctrl_source_t src);

/* ---------------- 查询接口 ---------------- */

bool    device_get_power(device_id_t id);
uint8_t device_get_level(device_id_t id);
esp_err_t device_get_color(device_id_t id, uint8_t *r, uint8_t *g, uint8_t *b);
const device_state_t *device_get_state(device_id_t id);

/** @brief 设备名（"led_living" ...），用于 MQTT / 语音 / 日志 */
const char *device_id_name(device_id_t id);

/** @brief 由名字反查设备，找不到返回 DEV_COUNT */
device_id_t device_from_name(const char *name);

/** @brief 把全部设备状态序列化成 JSON（写入 buf，返回写入长度） */
int device_snapshot_json(char *buf, size_t len);

/* ---------------- 事件订阅 ---------------- */

/** @brief 注册状态变化回调（只保留最后一个） */
esp_err_t device_register_cb(device_event_cb_t cb, void *user_data);

/** @brief 控制来源的名字，用于日志："key"/"voice"/"mqtt"/"auto"/"boot" */
const char *ctrl_source_name(ctrl_source_t src);

#ifdef __cplusplus
}
#endif
