#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// ======================================================================
// 输入：板载按键 KEY2 + 五位 AD 键盘
//
// 对应 ESP-IDF 工程的 components/BSP/KEY/key.c 和 components/BSP/ADKEY/adkey.c。
// 两个合并成一份，是因为它们都是"主循环轮询 + 回调报事件"，
// 分开写两套消抖反而啰嗦。
// ======================================================================

// 两个物理按键的编号
typedef enum {
    KEY_ID_1 = 0,
    KEY_ID_2,
    KEY_ID_MAX,
} key_id_t;

// 按键会报的事件
typedef enum {
    // 按下已消抖
    KEY_EVENT_DOWN = 0,
    // 松手抬起
    KEY_EVENT_UP,
    // 短按算单击
    KEY_EVENT_CLICK,
    // 按久了算长按
    KEY_EVENT_LONG_PRESS,
} key_event_t;

typedef void (*key_cb_t)(key_id_t id, key_event_t ev, void *user_data);

// 五位 AD 键盘的键号
typedef enum {
    // 键盘上印的 1
    ADKEY_1 = 0,
    // 键盘上印的 2
    ADKEY_2,
    // 键盘上印的 3
    ADKEY_3,
    // 键盘上印的 4
    ADKEY_4,
    // 键盘上印的 OK
    ADKEY_OK,
    ADKEY_NUM,
} adkey_id_t;

typedef enum {
    // 按下去了
    ADKEY_EVENT_DOWN = 0,
    // 松开了
    ADKEY_EVENT_UP,
    // 短按一下
    ADKEY_EVENT_CLICK,
    // 按住不放
    ADKEY_EVENT_LONG_PRESS,
} adkey_event_t;

typedef void (*adkey_cb_t)(adkey_id_t id, adkey_event_t ev, void *user_data);

// 把按键和键盘准备好
bool input_init(void);

// 登记回调，只留最后一个
void key_register_cb(key_cb_t cb, void *user_data);
void adkey_register_cb(adkey_cb_t cb, void *user_data);

// 主循环喊这个，消抖和长短按都在里面算
void input_poll(void);

// 这个键按着没
bool key_is_pressed(key_id_t id);
bool adkey_is_pressed(adkey_id_t id);

// 读现在多少伏
int adkey_raw_mv(void);

// 读按下那刻的电压
int adkey_last_mv(void);
