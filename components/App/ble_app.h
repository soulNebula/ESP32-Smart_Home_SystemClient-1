/*
 * 模块：
 *   手机蓝牙直连这条链路的口子。给 main.c 调用，具体活儿都在 ble_app.c 里干。
 *   它和上网那条链路是并排的两条路，不是谁顶替谁：手机走哪条都行，
 *   下发的都是同一套 JSON，认命令只有一份 —— 声明在 app_cmd.h，
 *   实现在 mqtt_app.c，本模块只把原文递过去，不自己再认一遍。
 *   上行由 app_link 广播分给两条链路，所以这边不用上层配合。
 *
 * 功能：
 *   起蓝牙
 *   看有没有手机连着
 *   看当前能收多大包
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：起蓝牙，不等广播 */
esp_err_t ble_app_start(void);

/* 功能：看有没有手机连着 */
bool ble_app_is_connected(void);

/* 功能：看当前能收多大包 */
int ble_app_get_mtu(void);

#ifdef __cplusplus
}
#endif
