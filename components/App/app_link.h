/*
 * 模块：
 *   上行的广播口子。要把一条消息发给手机时，只管往这里扔，它替你
 *   发给所有连着的路（网络一条、蓝牙一条），每条路怎么发由各自的
 *   回调决定。被 mqtt_app.c、ble_app.c 和 app_cmd.h 的实现调用；
 *   自己不碰硬件，只调各条路登记进来的发送函数。
 *
 *   好处是拼消息只写一份：以前网络和蓝牙各拼一套，改一个字段容易
 *   漏掉另一边，现在两条路共用同一份内容。以后要加新路（比如网页
 *   直连），只要写一个发送回调、开机登记一下，业务代码一行都不用动。
 *
 *   消息类型那个编号同时是蓝牙帧的第一个字节，定下来就不能改，
 *   不然手机那头会解析错位；网络这条不看编号（它按主题分）。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：上行消息的种类 */
typedef enum {
    APP_MSG_STATE  = 1,  /* 功能：全部设备的状态 */
    APP_MSG_SENSOR = 2,  /* 功能：传感器的数 */
    APP_MSG_ACK    = 3,  /* 功能：命令的结果 */
    APP_MSG_EVENT  = 4,  /* 功能：按键语音的事件 */
    APP_MSG_CONFIG = 5,  /* 功能：联动的几条线 */
} app_msg_type_t;

/* 功能：一条能发消息的路 */
typedef struct app_link {
    const char *name;   /* 功能：路的名字，日志看 */

    /* 功能：把一条消息发出去 */
    esp_err_t (*send)(app_msg_type_t type, const char *json, size_t len);

    /* 功能：这条路现在通不通 */
    bool (*is_connected)(void);
} app_link_t;

/* 功能：登记一条路 */
esp_err_t app_link_register(const app_link_t *link);

/* 功能：撤销一条路 */
esp_err_t app_link_unregister(const app_link_t *link);

/* 功能：发给所有通着的路 */
int app_link_broadcast(app_msg_type_t type, const char *json, size_t len);

/* 功能：有没有一条路通着 */
bool app_link_any_connected(void);

/* 功能：种类的名字，日志看 */
const char *app_msg_type_name(app_msg_type_t type);

#ifdef __cplusplus
}
#endif
