/*
 * 模块：
 *   按键的对外接口。管板上两个小按钮，被 main.c 和 adkey.c 那边用，
 *   内部自己起任务反复看电平，认出按下、抬起、单击、长按，
 *   再通过回调告诉上层去开灯关灯。
 *
 * 功能：
 *   把按键准备好
 *   登记按键回调
 *   查现在按着没有
 *   回调里别干重活
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：两个按键的编号 */
typedef enum {
    KEY_ID_1 = 0,
    KEY_ID_2,
    KEY_ID_MAX,
} key_id_t;

/* 功能：按键会报的事件 */
typedef enum {
    KEY_EVENT_DOWN = 0,   /* 功能：按下已消抖 */
    KEY_EVENT_UP,         /* 功能：松手抬起 */
    KEY_EVENT_CLICK,      /* 功能：短按算单击 */
    KEY_EVENT_LONG_PRESS, /* 功能：按久了算长按 */
} key_event_t;

typedef void (*key_cb_t)(key_id_t id, key_event_t ev, void *user_data);

/* 功能：把按键准备好 */
esp_err_t key_init(void);

/* 功能：登记按键回调 */
esp_err_t key_register_cb(key_cb_t cb, void *user_data);

/* 功能：查现在按着没有 */
bool key_is_pressed(key_id_t id);

#ifdef __cplusplus
}
#endif
