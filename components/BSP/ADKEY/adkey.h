#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    // 一共几个键位
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

// 按键了就通知外面
typedef void (*adkey_cb_t)(adkey_id_t id, adkey_event_t ev, void *user_data);

// 把键盘准备好
esp_err_t adkey_init(void);

// 登记按键回调
esp_err_t adkey_register_cb(adkey_cb_t cb, void *user_data);

// 这个键按着没
bool adkey_is_pressed(adkey_id_t id);

// 读现在多少伏
int adkey_raw_mv(void);

// 读按下那刻的电压
int adkey_last_mv(void);

#ifdef __cplusplus
}
#endif
