#pragma once

// ======================================================================
// 应用配置：功能开关 + 联网账号
//
// 对应 ESP-IDF 工程里的 sdkconfig.defaults（开关）
// 和 components/App/wifi_config.h（WiFi 账号）。
// ======================================================================

// ---- 功能开关：不想用哪个就把哪个改 0，代码不用动 ----
// 前三个各要一个第三方库，装法见 README「第二步」：
//   屏幕 -> U8g2（U8g2lib.h）
//   联网 -> PubSubClient（PubSubClient.h）
//   蓝牙 -> NimBLE-Arduino（NimBLEDevice.h）

// 屏幕（要装 U8g2 库）
#define APP_OLED_ENABLE         1
// 联网 + MQTT 上云（要装 PubSubClient 库）
#define APP_MQTT_ENABLE         1
// 蓝牙，手机 App 直连（要装 NimBLE-Arduino 库）
#define APP_BLE_ENABLE          1
// 语音模块 ASRPRO（只用串口，不用装库）
#define APP_VOICE_ENABLE        1
// 板载状态灯
#define APP_STATUS_LED_ENABLE   1

// ---- 这三个开关各要一个库，装法见 README「二」 ----
// 开着但没装库，编译会停在 `U8g2lib.h: No such file or directory` 这类报错上，
// 照 README 把库装上，或者把对应开关改成 0 就行。
//
// 别用 __has_include 在这儿自动把开关关掉：Arduino 先扫一遍源码里有没有
// #include <xxx.h> 才决定加不加载那个库，用 __has_include 一绕，库路径还没进来
// 就先判成"没有"，那个 #include 又被跳掉——结果是装了库也永远开不起来。

// 串口命令台的波特率
#define APP_SERIAL_BAUD         115200

// ---- WiFi 账号：填自己家的，必须是 2.4GHz ----
// 没填就安静地离线跑，BLE 和本地按键照常用
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

// 连不上重试几次就不等了，先干别的，后台接着连
#define APP_WIFI_MAX_RETRY      0

// ---- MQTT：默认用公共测试服务器，换成自己的也行 ----
#define APP_MQTT_BROKER         "broker.emqx.io"
#define APP_MQTT_PORT           1883
// 主题前缀：拼出来是 smarthome/<设备号>/state，和原工程一字不差
#define APP_MQTT_TOPIC_PREFIX   "smarthome"
// 心跳秒数
#define APP_MQTT_KEEPALIVE_S    60
// 留空就是不加密
#define APP_MQTT_USER           ""
#define APP_MQTT_PASSWORD       ""

// ---- BLE：手机 App 认这个名字 ----
// 蓝牙广播名的前缀，代码会在后面接上 "-<设备号>"，
// 广播出来就是 SmartHome-4d4a64 这种，和原工程一个样子
#define APP_BLE_NAME            "SmartHome"

// ---- 上报用的设备编号：主题里带上它，多个板子不打架 ----
// 留空就自动用芯片 MAC 后三位拼一个
#define APP_DEVICE_ID           ""
