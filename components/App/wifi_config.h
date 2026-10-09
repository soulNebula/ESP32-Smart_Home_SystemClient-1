// ============================ WiFi 账号配置 ============================
// 改下面两行就能用，改完直接重新编译烧录
//
//   APP_WIFI_SSID      路由器 WiFi 名称，必须是 2.4GHz（ESP32-S3 不支持 5GHz）
//   APP_WIFI_PASSWORD  对应的密码；开放网络写 ""
//
// ★ 本文件受版本控制：提交代码 / 打包交付前，请把下面两行改回占位符，
//   别把家里或现场的 WiFi 密码带进仓库和交付包
//
// 改完怎么编译烧录：
//   Linux   :  tools/build/build.sh
//   Windows :  tools\build\build.ps1 -Task build
// ======================================================================

#pragma once

// 例子：#define APP_WIFI_SSID       "YOUR_WIFI_SSID"
#define APP_WIFI_SSID       "YOUR_WIFI_SSID"

// 例子：#define APP_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define APP_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
