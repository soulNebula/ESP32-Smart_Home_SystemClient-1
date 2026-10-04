/*
 * 模块：
 *   五位 AD 键盘。一个信号脚读电压就能认出按了哪个键，按键事件给 main.c
 *   和界面导航用；被 board.c 开机初始化，判定规则放在 adkey_logic.h，
 *   具体实现在 adkey.c。
 *
 * 功能：
 *   开机起扫描任务
 *   登记按键回调
 *   查某个键按着没
 *   读现在的电压
 *   读按下那刻电压
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADKEY_1 = 0,      /* 功能：键盘上印的 1 */
    ADKEY_2,          /* 功能：键盘上印的 2 */
    ADKEY_3,          /* 功能：键盘上印的 3 */
    ADKEY_4,          /* 功能：键盘上印的 4 */
    ADKEY_OK,         /* 功能：键盘上印的 OK */
    ADKEY_NUM,        /* 功能：一共几个键位 */
} adkey_id_t;

typedef enum {
    ADKEY_EVENT_DOWN = 0,      /* 功能：按下去了 */
    ADKEY_EVENT_UP,            /* 功能：松开了 */
    ADKEY_EVENT_CLICK,         /* 功能：短按一下 */
    ADKEY_EVENT_LONG_PRESS,    /* 功能：按住不放 */
} adkey_event_t;

/* 功能：按键了就通知外面 */
typedef void (*adkey_cb_t)(adkey_id_t id, adkey_event_t ev, void *user_data);

/* 功能：把键盘准备好 */
esp_err_t adkey_init(void);

/* 功能：登记按键回调 */
esp_err_t adkey_register_cb(adkey_cb_t cb, void *user_data);

/* 功能：这个键按着没 */
bool adkey_is_pressed(adkey_id_t id);

/* 功能：读现在多少伏 */
int adkey_raw_mv(void);

/* 功能：读按下那刻的电压 */
int adkey_last_mv(void);

#ifdef __cplusplus
}
#endif
