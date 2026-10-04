/**
 * @file  wifi_sta.h
 * @brief WiFi Station 连接管理（带自动重连）
 *
 * SSID / 密码来自 menuconfig（main/Kconfig.projbuild），默认值见
 * CONFIG_APP_WIFI_SSID / CONFIG_APP_WIFI_PASSWORD。
 * 连不上不会卡住系统：MQTT 会一直重试，其余本地功能（语音/按键/联动）照常工作。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 netif + event 循环 + WiFi STA 并开始连接
 * @note 非阻塞，立刻返回。是否连通用 wifi_is_connected() 查。
 */
esp_err_t wifi_init_sta(void);

/** @brief 是否已拿到 IP */
bool wifi_is_connected(void);

/** @brief 阻塞等待连上（或超时） */
void wifi_wait_connected(uint32_t timeout_ms);

/** @brief 取 IP 字符串，如 "192.168.1.23"；失败写入 "0.0.0.0" */
esp_err_t wifi_get_ip_str(char *buf, size_t len);

/** @brief 取 RSSI 信号强度（dBm），未连接返回 0 */
int8_t wifi_get_rssi(void);

/** @brief 取 MAC 后 3 字节十六进制串（如 "a1b2c3"），用作 MQTT 唯一 ID */
esp_err_t wifi_get_mac_suffix(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
