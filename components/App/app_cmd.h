/*
 * 模块：
 *   收命令这一段的公用接口。手机不管走网络还是蓝牙，下发的都是同一套
 *   命令，所以解析只留一份，两条路都调这里。接口在本文件，
 *   实现放在 mqtt_app.c（跟那边的状态绑得紧）；解析完向下调设备总状态表
 *   （device_model.c）动手，回执经 app_link.c 发回两条路。
 *
 *   命令里带着来源，一路传到设备总状态表：日志里能看出是谁改的，
 *   也决定这次算不算人工操作（人工操作会压住自动联动一会儿）。
 *   所以走网络传网络、走蓝牙传蓝牙，别图省事混着用。
 */
#pragma once

#include "device_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：处理一条控制命令 */
void app_cmd_handle_json(const char *json, int len, ctrl_source_t src);

/* 功能：处理一条改配置的命令 */
void app_cmd_handle_config_json(const char *json, int len, ctrl_source_t src);

#ifdef __cplusplus
}
#endif
