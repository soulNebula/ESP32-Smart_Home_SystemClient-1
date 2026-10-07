#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 上电连上路由器
esp_err_t wifi_init_sta(void);

// 拿到地址没有
bool wifi_is_connected(void);

// 等一会儿连上没
void wifi_wait_connected(uint32_t timeout_ms);

// 要一下自己的地址
esp_err_t wifi_get_ip_str(char *buf, size_t len);

// 看看信号多强
int8_t wifi_get_rssi(void);

// 取网卡号后半段当编号
esp_err_t wifi_get_mac_suffix(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
