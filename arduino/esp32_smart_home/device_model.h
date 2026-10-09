#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ======================================================================
// 设备状态表：全屋设备都在这儿，谁改都走这里
//
// 对应 ESP-IDF 工程的 components/App/device_model.c。
// 原工程返回 esp_err_t，Arduino 这份统一返回 bool：true=成功。
// ======================================================================

// 全屋设备清单
typedef enum {
    // 客厅灯
    DEV_LED_LIVING = 0,
    // 厨房灯
    DEV_LED_KITCHEN,
    // 卧室灯
    DEV_LED_BEDROOM,
    // 浴室灯
    DEV_LED_BATH,
    // 风扇，可调速
    DEV_FAN,
    // 窗户，0关100开
    DEV_WINDOW,
    // 门
    DEV_DOOR,
    // 窗帘，舵机停用后命令会被拒
    DEV_CURTAIN,
    DEV_COUNT,
} device_id_t;

// 谁改的，人工还是自动
typedef enum {
    // 上电初始化
    SRC_BOOT = 0,
    // 板载按键
    SRC_LOCAL_KEY,
    // 语音
    SRC_VOICE,
    // 手机走网络
    SRC_MQTT,
    // 手机走蓝牙
    SRC_BLE,
    // 自动联动规则
    SRC_AUTO,
    // 按键自检模式
    SRC_SELFTEST,
} ctrl_source_t;

// 一台设备的状态
typedef struct {
    // 开关，舵机过半算开
    bool     power;
    // 亮度转速或开合度
    uint8_t  level;
    // 灯的颜色，这版 LED 模块是白光，改了只记状态
    uint8_t  r, g, b;
} device_state_t;

// 状态一变就通知上层
typedef void (*device_event_cb_t)(device_id_t id, ctrl_source_t src, void *user_data);

// 上电把设备都置成关
bool device_model_init(void);

// 开关一台设备
bool device_set_power(device_id_t id, bool on, ctrl_source_t src);

// 开的变关，关的变开
bool device_toggle(device_id_t id, ctrl_source_t src);

// 调亮度转速或开合度
bool device_set_level(device_id_t id, uint8_t percent, ctrl_source_t src);

// 调灯的颜色，只有灯行
bool device_set_color(device_id_t id, uint8_t r, uint8_t g, uint8_t b, ctrl_source_t src);

// 全关，舵机也归位
bool device_all_off(ctrl_source_t src);

bool    device_get_power(device_id_t id);
uint8_t device_get_level(device_id_t id);

// 设备名字，上报日志用
const char *device_id_name(device_id_t id);

// 名字反查设备，找不到给 DEV_COUNT
device_id_t device_from_name(const char *name);

// 全部状态拼成一段 JSON
int device_snapshot_json(char *buf, size_t len);

// 登记状态变化的回调，只留最后一个
void device_register_cb(device_event_cb_t cb, void *user_data);

// 来源的名字，日志里看
const char *ctrl_source_name(ctrl_source_t src);
