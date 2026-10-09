#pragma once

#include <stdbool.h>
#include <stddef.h>

// ======================================================================
// 联网：WiFi + MQTT 上云
//
// 对应 ESP-IDF 工程的 components/App/wifi_sta.c + mqtt_app.c + mqtt_protocol.c。
// 主题和报文格式和原工程一样，手机 App / MQTTX 不用改。
// ======================================================================

// 把 WiFi 和 MQTT 都准备好，WiFi 没填账号就安静地离线跑
bool net_mqtt_init(void);

// 主循环喊这个，重连和收报文都在里面
void net_mqtt_poll(void);

bool net_mqtt_wifi_is_connected(void);
bool net_mqtt_is_connected(void);

// 这几条往上发，状态一变就叫
void net_mqtt_publish_state(void);
void net_mqtt_publish_sensor(void);
void net_mqtt_publish_config(void);
void net_mqtt_publish_event(const char *event);
void net_mqtt_publish_ack(const char *action, bool ok, const char *msg);

// 查到本机 IP 给外面看，没连上给 "0.0.0.0"
const char *net_mqtt_ip_str(void);

// WiFi 信号强度，没连上给 0
int net_mqtt_rssi(void);

// 拼那个前缀，主题和 client_id 都用它
const char *net_mqtt_device_id(void);
