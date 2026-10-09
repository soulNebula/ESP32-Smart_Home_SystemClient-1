#pragma once

#include <stdbool.h>

// ======================================================================
// 蓝牙：手机 App 直连
//
// 对应 ESP-IDF 工程的 components/App/ble_app.c。
// 服务号和特征号和原工程保持一致，手机 App 不用改。
// 要装 NimBLE-Arduino 库，不想用就把 app_config.h 里 APP_BLE_ENABLE 改成 0。
// ======================================================================

// 把蓝牙准备好，没连上也不影响别的
bool net_ble_init(void);

// 主循环喊这个
void net_ble_poll(void);

bool net_ble_is_connected(void);

// 往手机推，手机订阅了才真发出去
void net_ble_notify_state(void);
void net_ble_notify_sensor(void);
void net_ble_notify_config(void);
