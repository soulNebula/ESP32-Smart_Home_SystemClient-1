#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 起蓝牙，不等广播
esp_err_t ble_app_start(void);

// 看有没有手机连着
bool ble_app_is_connected(void);

// 看当前能收多大包
int ble_app_get_mtu(void);

#ifdef __cplusplus
}
#endif
