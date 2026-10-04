/**
 * @file  key.h
 * @brief 本地按键（KEY1=GPIO10 / KEY2=GPIO11，按下接地）
 *
 * 内部起一个扫描任务做软件消抖，识别：按下 / 抬起 / 单击 / 长按。
 * 通过回调把事件抛给 App 层，App 再翻译成"开灯/关灯/切换模式"等业务动作。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEY_ID_1 = 0,
    KEY_ID_2,
    KEY_ID_MAX,
} key_id_t;

typedef enum {
    KEY_EVENT_DOWN = 0,   /**< 按下（已消抖） */
    KEY_EVENT_UP,         /**< 抬起 */
    KEY_EVENT_CLICK,      /**< 单击（抬起且短于长按阈值） */
    KEY_EVENT_LONG_PRESS, /**< 长按达到 BSP_KEY_LONG_PRESS_MS */
} key_event_t;

typedef void (*key_cb_t)(key_id_t id, key_event_t ev, void *user_data);

/** @brief 初始化按键 GPIO 并启动扫描任务（幂等） */
esp_err_t key_init(void);

/** @brief 注册回调（只保留最后一个注册者，够用） */
esp_err_t key_register_cb(key_cb_t cb, void *user_data);

/** @brief 当前是否处于按下状态 */
bool key_is_pressed(key_id_t id);

#ifdef __cplusplus
}
#endif
