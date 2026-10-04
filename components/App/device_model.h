/*
 * 模块：
 *   设备总状态表。全屋每台设备（四路灯、风扇、窗、门、帘）的开关、
 *   亮度、颜色都只记在这一份里，是本项目唯一的真相来源：语音、按键、
 *   手机、自动联动都只改它，界面刷新、上报手机、联动判断也只读它，
 *   不会各处各存一套对不上。被 automation.c、mqtt_app.c、ble_app.c、
 *   main.c 调用；自己向下调 led、fan、servo 三个硬件模块动真设备。
 *
 *   名字表跟设备清单一一对应；按名字反查时，"all" 和认不出来的名字都
 *   返回表尾那个值，上层用一句话就能判"是全部还是非法"。
 *
 * 功能：
 *   记住每台设备的状态
 *   改状态时顺手动硬件
 *   改完通知界面和手机
 *   记下这次是谁改的
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：全屋设备清单 */
typedef enum {
    DEV_LED_LIVING = 0,  /* 功能：客厅灯带 */
    DEV_LED_KITCHEN,     /* 功能：厨房灯带 */
    DEV_LED_BEDROOM,     /* 功能：卧室灯带 */
    DEV_LED_BATH,        /* 功能：浴室灯带 */
    DEV_FAN,             /* 功能：风扇，可调速 */
    DEV_WINDOW,          /* 功能：窗户，0关100开 */
    DEV_DOOR,            /* 功能：门 */
    DEV_CURTAIN,         /* 功能：窗帘 */
    DEV_COUNT,
} device_id_t;

/* 功能：谁改的，人工还是自动 */
typedef enum {
    SRC_BOOT = 0,   /* 功能：上电初始化 */
    SRC_LOCAL_KEY,  /* 功能：板载按键 */
    SRC_VOICE,      /* 功能：语音 */
    SRC_MQTT,       /* 功能：手机走网络 */
    SRC_BLE,        /* 功能：手机走蓝牙 */
    SRC_AUTO,       /* 功能：自动联动规则 */
    SRC_SELFTEST,   /* 功能：按键自检模式 */
} ctrl_source_t;

/* 功能：一台设备的状态 */
typedef struct {
    bool     power;     /* 功能：开关，舵机过半算开 */
    uint8_t  level;     /* 功能：亮度转速或开合度 */
    uint8_t  r, g, b;   /* 功能：灯的颜色，别的没用 */
    uint32_t change_cnt;/* 功能：改过多少次，调试看 */
} device_state_t;

/* 功能：状态一变就通知上层 */
typedef void (*device_event_cb_t)(device_id_t id, ctrl_source_t src, void *user_data);

/* 功能：上电把设备都置成关 */
esp_err_t device_model_init(void);

/* 功能：开关一台设备 */
esp_err_t device_set_power(device_id_t id, bool on, ctrl_source_t src);

/* 功能：开的变关，关的变开 */
esp_err_t device_toggle(device_id_t id, ctrl_source_t src);

/* 功能：调亮度转速或开合度 */
esp_err_t device_set_level(device_id_t id, uint8_t percent, ctrl_source_t src);

/* 功能：调灯带颜色，只有灯行 */
esp_err_t device_set_color(device_id_t id, uint8_t r, uint8_t g, uint8_t b, ctrl_source_t src);

/* 功能：全关，舵机也归位 */
esp_err_t device_all_off(ctrl_source_t src);

bool    device_get_power(device_id_t id);
uint8_t device_get_level(device_id_t id);
esp_err_t device_get_color(device_id_t id, uint8_t *r, uint8_t *g, uint8_t *b);
const device_state_t *device_get_state(device_id_t id);

/* 功能：设备名字，上报日志用 */
const char *device_id_name(device_id_t id);

/* 功能：名字反查设备 */
device_id_t device_from_name(const char *name);

/* 功能：全部状态拼成一段 JSON */
int device_snapshot_json(char *buf, size_t len);

/* 功能：登记状态变化的回调 */
esp_err_t device_register_cb(device_event_cb_t cb, void *user_data);

/* 功能：来源的名字，日志里看 */
const char *ctrl_source_name(ctrl_source_t src);

#ifdef __cplusplus
}
#endif
