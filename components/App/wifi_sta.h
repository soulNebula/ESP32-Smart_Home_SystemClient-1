/*
 * 模块：
 *   连路由器。名字和密码在 menuconfig 里填，上电后自己去连，
 *   掉线了按设定一直重试。被 main.c 调用；连上之后网络那条路
 *   （mqtt_app.c）才有得用。连不上也不耽误事儿：语音、按键、
 *   联动这些本地活儿照常干。
 *
 * 功能：
 *   上电连上路由器
 *   问问连上没
 *   等一会儿，等不到也走
 *   报出地址和信号强弱
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：上电连上路由器 */
esp_err_t wifi_init_sta(void);

/* 功能：拿到地址没有 */
bool wifi_is_connected(void);

/* 功能：等一会儿连上没 */
void wifi_wait_connected(uint32_t timeout_ms);

/* 功能：要一下自己的地址 */
esp_err_t wifi_get_ip_str(char *buf, size_t len);

/* 功能：看看信号多强 */
int8_t wifi_get_rssi(void);

/* 功能：取网卡号后半段当编号 */
esp_err_t wifi_get_mac_suffix(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
