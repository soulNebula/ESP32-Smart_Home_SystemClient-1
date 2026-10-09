#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 起蓝牙，不等广播
esp_err_t ble_app_start(void);

#ifdef __cplusplus
}
#endif
