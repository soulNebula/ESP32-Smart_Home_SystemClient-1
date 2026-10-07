#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 两个按键的编号
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

// 把按键准备好
esp_err_t key_init(void);

// 登记按键回调
esp_err_t key_register_cb(key_cb_t cb, void *user_data);

// 查现在按着没有
bool key_is_pressed(key_id_t id);

#ifdef __cplusplus
}
#endif
