#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 上行消息的种类
typedef enum {
    // 全部设备的状态
    APP_MSG_STATE  = 1,
    // 传感器的数
    APP_MSG_SENSOR = 2,
    // 命令的结果
    APP_MSG_ACK    = 3,
    // 按键语音的事件
    APP_MSG_EVENT  = 4,
    // 联动的几条线
    APP_MSG_CONFIG = 5,
} app_msg_type_t;

// 一条能发消息的路
typedef struct app_link {
    // 路的名字，日志看
    const char *name;

    // 把一条消息发出去
    esp_err_t (*send)(app_msg_type_t type, const char *json, size_t len);

    // 这条路现在通不通
    bool (*is_connected)(void);
} app_link_t;

// 登记一条路
esp_err_t app_link_register(const app_link_t *link);

// 撤销一条路
esp_err_t app_link_unregister(const app_link_t *link);

// 发给所有通着的路
int app_link_broadcast(app_msg_type_t type, const char *json, size_t len);

// 有没有一条路通着
bool app_link_any_connected(void);

// 种类的名字，日志看
const char *app_msg_type_name(app_msg_type_t type);

#ifdef __cplusplus
}
#endif
