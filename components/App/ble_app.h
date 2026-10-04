/**
 * @file  ble_app.h
 * @brief BLE (NimBLE) GATT 服务端 —— 手机 App 的第二条控制链路
 *
 * ===========================================================================
 *  它和 MQTT 是什么关系？
 * ===========================================================================
 *  【并列】关系，不是替代关系。两者共用同一套上行 JSON 和同一套下行命令：
 *
 *      手机 App ──MQTT──┐
 *                       ├─► app_cmd_handle_*(src) ─► device_model ─► 硬件
 *      手机 App ──BLE───┘                                   │
 *                                                           ▼
 *                           app_link_broadcast() ──┬──► MQTT publish(<base>/state ...)
 *                                                  └──► BLE notify(0x01 + JSON)
 *
 *  所以本模块【不需要】任何上层业务代码配合：只要在开机时 ble_app_start()，
 *  之后 device_model 的状态变化、传感器数据、ack、event、config 全都会自动
 *  通过 BLE 推给手机（由 app_link 注册表转发）。详见 app_link.h。
 *
 * ===========================================================================
 *  对外可见的接口就只有下面三个（都做了"没连上也不报错"的处理）
 * ===========================================================================
 *  ⚠ 与手机端的契约（广播名 / Service UUID / 两个特征 / 帧格式）写死在
 *    ble_app.c 顶部的注释里。改任何一条都必须同步改 android 端与文档，
 *    否则手机连得上却收不到数据。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 BLE GATT 服务端（幂等，非阻塞）
 *
 * 做的事情：初始化 NimBLE 协议栈 → 注册 GATT 服务 → 把 BLE 作为一条链路
 * 注册进 app_link → 起 NimBLE 主机任务 → 开始广播。
 *
 * @return ESP_OK            已启动（或之前已经启动过）
 *         ESP_ERR_NOT_SUPPORTED 编译时没打开蓝牙栈（CONFIG_BT_ENABLED=n）
 *                              或本模块被 CONFIG_APP_BLE_ENABLE=n 关掉了
 *         其它                 NimBLE 初始化/注册失败
 *
 * @note 本函数【不会】等到广播真正开始才返回（广播是在 NimBLE 主机任务里
 *       启动的），所以调用方可继续做别的事。失败只影响蓝牙控制，
 *       WiFi / MQTT / 本地控制完全不受影响。
 */
esp_err_t ble_app_start(void);

/** @brief 当前是否有手机连着（用来判断值不值得拼 JSON、日志/OLED 显示） */
bool ble_app_is_connected(void);

/**
 * @brief 当前协商出来的 ATT MTU
 * @return 已连接时有意义的值（默认 23，协商后通常 512）；未连接返回 0
 */
int ble_app_get_mtu(void);

#ifdef __cplusplus
}
#endif
