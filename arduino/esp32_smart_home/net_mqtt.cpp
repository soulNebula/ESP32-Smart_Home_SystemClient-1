// ======================================================================
// 联网：WiFi + MQTT 上云（Arduino 版）
//
// 对应 ESP-IDF 工程这三个文件：
//   components/App/wifi_sta.c      —— 连 WiFi、掉线重连、报 IP
//   components/App/mqtt_app.c      —— 遗嘱、订阅、收命令、各条上报
//   components/App/mqtt_protocol.c —— 主题怎么拼
// 主题和报文和原版一样，手机 App / MQTTX 那边不用改。
//
// 和原版说清楚不一样的几处：
//   1. 原版 esp-mqtt 跑在自己的任务里，这版 PubSubClient 只能主循环喊 poll()，
//      所以收发都在主循环做；WiFi 首次连接也不死等（原版等十秒），
//      全交给节流重连慢慢来，主循环一点都不卡。
//   2. PubSubClient 发不了 QoS1，原版那几条 QoS1 的这儿降成 QoS0，
//      retain 一个没改，手机端看不出差别。
//   3. 拼报文原来用 cJSON，这版手写 snprintf，省得再拖一个库进来。
//   4. 收上来的 payload PubSubClient 不给结尾零，先抄一份再解析。
//   5. 板载状态灯原版是 wifi_sta.c / mqtt_app.c 顺手点的，这版归主程序
//      status_led_poll() 统一按这两个查询口上色，这儿就不抢了。
//   6. 命令回执原版是在 app_cmd_handle_json 里发的，这版那函数只管执行、
//      返回成没成，回执由这边代发。
//   7. 设备号两个分支都算得出来（联网关掉也给），蓝牙那边要拿它拼设备名。
//
// app_config.h 里 APP_MQTT_ENABLE 为 0 时，下面整块不参与编译，
// 连 PubSubClient.h 都不 include，所以没装库也照样编过。
// ======================================================================

#include "app_config.h"
#include "net_mqtt.h"

// 关掉联网时也要打一行说明，所以 Arduino.h 放在外头
#include <Arduino.h>

// 设备号算 MAC 用，两个分支都要算（蓝牙那边拼设备名也要），所以放外头
#include <esp_mac.h>
#include <esp_random.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------
// 设备号：联网开着关着都得有个号，蓝牙那边也要用它拼名字
// ----------------------------------------------------------------------

// 设备号那一格多大，原版 MQTT_UID_MAX 是 32
#define NET_MQTT_UID_LEN         32
// 六个字加个零，原版也是按 len < 7 卡
#define NET_MQTT_UID_MIN_LEN     7

// 算一次就存着，谁问都给这一个
static char s_uid[NET_MQTT_UID_LEN] = { 0 };

// 取这台板的号：填了 APP_DEVICE_ID 就用它，没填拿 MAC 后三个字节拼
static bool build_uid(char *buf, size_t len) {
    uint8_t mac[6] = { 0 };

    if (buf == NULL || len < NET_MQTT_UID_MIN_LEN) {
        Serial.printf("mqtt: 放设备号的格子太小（%u 字节）\n", (unsigned)len);
        return false;
    }

    if (APP_DEVICE_ID[0] != '\0') {
        if (strlen(APP_DEVICE_ID) < len) {
            snprintf(buf, len, "%s", APP_DEVICE_ID);
            return true;
        }
        // 太长了就别硬塞，改用 MAC 拼，免得主题和客户端名被撑爆
        Serial.printf("mqtt: APP_DEVICE_ID \"%s\" 太长（最多 %u 字节），改用 MAC 后三位\n",
                      APP_DEVICE_ID, (unsigned)(len - 1));
    }

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        // MAC 都读不到就随手给一个，好歹能连上，原版也是这么兜的
        snprintf(buf, len, "%06x", (unsigned)(esp_random() & 0xFFFFFFu));
        Serial.printf("mqtt: 读不到 MAC，临时设备号 %s\n", buf);
        return true;
    }

    // 后三个字节拼六位十六进制，原版是 esp32sh-4d4a64 这种风格
    snprintf(buf, len, "%02x%02x%02x", (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    return true;
}

// 没算过就算一次，算不出来也得给个能用的名字，别让外面拿到空串
static const char *uid_ensure(void) {
    if (s_uid[0] == '\0') {
        if (!build_uid(s_uid, sizeof(s_uid))) {
            snprintf(s_uid, sizeof(s_uid), "%s", "esp32sh");
        }
    }
    return s_uid;
}

#if APP_MQTT_ENABLE

#include <WiFi.h>
#include <PubSubClient.h>

#include <stdlib.h>

#include "app_cmd.h"
#include "automation.h"
#include "device_model.h"
#include "sensor.h"

// 联网开关开着又没装 PubSubClient，编译会停在
// "PubSubClient.h: No such file or directory"：照 README 把库装上，
// 或者把 app_config.h 里的 APP_MQTT_ENABLE 改成 0；
// 改成 0 之后下面整块都不参与编译，连这个头都不用找

// ----------------------------------------------------------------------
// 协议常量：主题前缀、遗嘱报文、各条主题尾巴
// ----------------------------------------------------------------------

// 主题前缀。原工程是 sdkconfig 里的 CONFIG_APP_MQTT_TOPIC_PREFIX="smarthome"，
// app_config.h 里没这一项，想改就在 app_config.h 里加个同名的
#ifndef APP_MQTT_TOPIC_PREFIX
#define APP_MQTT_TOPIC_PREFIX   "smarthome"
#endif

// 心跳秒数，原工程 CONFIG_APP_MQTT_KEEPALIVE_S=60
#ifndef APP_MQTT_KEEPALIVE_S
#define APP_MQTT_KEEPALIVE_S    60
#endif

// 在线和离线两个固定报文，名字照 mqtt_protocol.h 抄
#define MQTT_PAYLOAD_ONLINE     "online"
#define MQTT_PAYLOAD_OFFLINE    "offline"

// 主题尾巴，逐条对着 mqtt_protocol.c 的 build_topic 抄，一个字都不差
// 状态：整屋设备 + 网络那几项
#define MQTT_SUFFIX_STATE        "/state"
// 测量：温湿度光照雨滴
#define MQTT_SUFFIX_SENSOR       "/sensor"
// 在线：遗嘱也挂在这条上
#define MQTT_SUFFIX_AVAILABILITY "/availability"
// 回执：一条命令干完回一句
#define MQTT_SUFFIX_ACK          "/ack"
// 事件：本地按键、语音这些
#define MQTT_SUFFIX_EVENT        "/event"
// 命令：手机下发的开关
#define MQTT_SUFFIX_CMD          "/cmd"
// 命令通配：订这条才收得到 cmd/xxx
#define MQTT_SUFFIX_CMD_WILDCARD "/cmd/#"
// 阈值：读写都是这条
#define MQTT_SUFFIX_CONFIG       "/config"
// 查询：要一份现状
#define MQTT_SUFFIX_GET          "/get"

// ----------------------------------------------------------------------
// 缓冲区大小：对着原版那几个宏来
// ----------------------------------------------------------------------

// 放拼好的主题，原版 TOPIC_BUF_LEN 就是 128
#define NET_MQTT_TOPIC_LEN       128
// 放设备快照，原版 SNAP_BUF_LEN 是 1024
#define NET_MQTT_SNAP_LEN        1024
// 快照再补上 auto/rssi/ip/uptime 那几项，留点富余
#define NET_MQTT_STATE_LEN       1152
// 收下来的报文先抄一份，PubSubClient 给的指针不带结尾零
#define NET_MQTT_RX_LEN          1024
// 收回来的阈值报文，原版也是按 256 卡
#define NET_MQTT_CFG_RX_LEN      256
// 发出去的阈值报文，原版 automation_cfg_json 那边就是 512
#define NET_MQTT_CFG_LEN         512
// 回执报文
#define NET_MQTT_ACK_LEN         192
// 事件报文
#define NET_MQTT_EVENT_LEN       128
// 传感器报文
#define NET_MQTT_SENSOR_LEN      192
// 客户端名，原版 CLIENT_ID_LEN 是 48
#define NET_MQTT_CLIENT_ID_LEN   48

// ----------------------------------------------------------------------
// 重连和超时的参数
// ----------------------------------------------------------------------

// PubSubClient 默认收发缓冲只有 256 字节，状态报文装不下，先撑开；
// 现在这份状态报文撑死四百来字节，1024 有两倍多富余
#define NET_MQTT_BUF_LEN         1024
// TCP 先自己用短超时捅一下，单位毫秒；捅不开就不麻烦 broker 了
#define NET_MQTT_TCP_TIMEOUT_MS  3000
// 等 CONNACK 最多几秒（PubSubClient 的 socketTimeout，单位秒），别卡主循环
#define NET_MQTT_SOCKET_TIMEOUT_S 3
// WiFi 重连节流：五秒试一次，不每圈都喊，也不刷屏
#define NET_MQTT_WIFI_RETRY_MS   5000
// MQTT 重连节流，原版 network.reconnect_timeout_ms = 5000
#define NET_MQTT_RETRY_MIN_MS    5000
// 连不上就退避，封顶一分钟，别越等越久到天荒地老
#define NET_MQTT_RETRY_MAX_MS    60000

// ----------------------------------------------------------------------
// 模块状态
// ----------------------------------------------------------------------

// 初始化过就不再来一遍
static bool s_inited = false;
// WiFi 账号没填，安静离线跑，poll 里什么都不做
static bool s_wifi_offline = false;
// 连上 broker 才置真
static bool s_mqtt_connected = false;
// 上一次看到的网通没通，用来认状态变化
static bool s_wifi_up = false;
// 掉线原因只有事件里给，事件任务写、主循环读
static volatile uint8_t s_disc_reason = 0;
// 上次试连 WiFi / MQTT 是什么时候
static uint32_t s_wifi_try_ms = 0;
static uint32_t s_mqtt_try_ms = 0;
// 这次没连上，下次等多会儿（毫秒）
static uint32_t s_mqtt_backoff_ms = NET_MQTT_RETRY_MIN_MS;
// 试了几回，日志里报个数
static int s_wifi_retry = 0;

// 本机地址，没连上就是 0.0.0.0
static char s_ip_str[16] = "0.0.0.0";

// 客户端名和各条主题，开机拼一次就不动了（设备号在上面算好了）
static char s_client_id[NET_MQTT_CLIENT_ID_LEN] = { 0 };
static char s_topic_state[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_sensor[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_availability[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_ack[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_event[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_cmd[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_cmd_wild[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_config[NET_MQTT_TOPIC_LEN] = { 0 };
static char s_topic_get[NET_MQTT_TOPIC_LEN] = { 0 };

// 自己刚发出去的阈值，认回显用，别自己改自己
static char s_last_pub_config[NET_MQTT_CFG_LEN] = { 0 };

// 收下来的报文抄这儿，PubSubClient 那个指针不带结尾零
static char s_rx[NET_MQTT_RX_LEN];

// 一条 TCP 连接给 MQTT 用，就按题目要求这么搭
static WiFiClient s_net_client;
static PubSubClient s_mqtt(s_net_client);

// ----------------------------------------------------------------------
// 小工具
// ----------------------------------------------------------------------

// 跳过空白，JSON 从别处复制过来常常带空格换行
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

// 跳过一整段字符串，p 指着开头那个引号，返回收尾引号后面那个位置
static const char *skip_json_string(const char *p) {
    p++;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            // 转义的两个字符一起跳
            p += 2;
            continue;
        }
        if (*p == '"') {
            return p + 1;
        }
        p++;
    }
    return p;
}

// 跳过一整个值，字符串、数字、嵌套的对象数组都认
static const char *skip_json_value(const char *p) {
    if (*p == '"') {
        return skip_json_string(p);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;

        while (*p != '\0') {
            if (*p == '"') {
                p = skip_json_string(p);
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth <= 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return p;
    }
    // 剩下的就是数字、true、false、null 这些，碰到分隔符就停
    while (*p != '\0' && *p != ',' && *p != '}') {
        p++;
    }
    return p;
}

// ----------------------------------------------------------------------
// 主题
// ----------------------------------------------------------------------

// 前缀加号再接尾巴，和 mqtt_protocol.c 的 build_topic 一个写法
static bool build_topic(char *buf, size_t len, const char *suffix) {
    int n;

    if (suffix != NULL) {
        n = snprintf(buf, len, "%s/%s%s", APP_MQTT_TOPIC_PREFIX, s_uid, suffix);
    } else {
        n = snprintf(buf, len, "%s/%s", APP_MQTT_TOPIC_PREFIX, s_uid);
    }

    if (n < 0 || (size_t)n >= len) {
        Serial.printf("mqtt: 主题 \"%s/%s%s\" 放不进 %u 字节的格子\n",
                      APP_MQTT_TOPIC_PREFIX, s_uid, (suffix != NULL) ? suffix : "",
                      (unsigned)len);
        return false;
    }
    return true;
}

// 开机把九条主题拼一遍，后面直接拿来用
static bool build_all_topics(void) {
    return build_topic(s_topic_state, sizeof(s_topic_state), MQTT_SUFFIX_STATE)
        && build_topic(s_topic_sensor, sizeof(s_topic_sensor), MQTT_SUFFIX_SENSOR)
        && build_topic(s_topic_availability, sizeof(s_topic_availability), MQTT_SUFFIX_AVAILABILITY)
        && build_topic(s_topic_ack, sizeof(s_topic_ack), MQTT_SUFFIX_ACK)
        && build_topic(s_topic_event, sizeof(s_topic_event), MQTT_SUFFIX_EVENT)
        && build_topic(s_topic_cmd, sizeof(s_topic_cmd), MQTT_SUFFIX_CMD)
        && build_topic(s_topic_cmd_wild, sizeof(s_topic_cmd_wild), MQTT_SUFFIX_CMD_WILDCARD)
        && build_topic(s_topic_config, sizeof(s_topic_config), MQTT_SUFFIX_CONFIG)
        && build_topic(s_topic_get, sizeof(s_topic_get), MQTT_SUFFIX_GET);
}

// 看主题尾巴对不对，原版 topic_endswith 一个路子
static bool topic_endswith(const char *topic, const char *suffix) {
    size_t tl;
    size_t sl;

    if (topic == NULL || suffix == NULL) {
        return false;
    }

    tl = strlen(topic);
    sl = strlen(suffix);
    if (tl < sl) {
        return false;
    }
    return memcmp(topic + tl - sl, suffix, sl) == 0;
}

// 认出命令主题：cmd 本身和 cmd/xxx 都算，原版 topic_is_cmd 就是这么判的
static bool topic_is_cmd(const char *topic) {
    size_t cl;

    if (topic == NULL) {
        return false;
    }

    cl = strlen(s_topic_cmd);
    if (cl == 0) {
        return false;
    }
    if (strncmp(topic, s_topic_cmd, cl) != 0) {
        return false;
    }
    return (topic[cl] == '\0') || (topic[cl] == '/');
}

// ----------------------------------------------------------------------
// 往外发
// ----------------------------------------------------------------------

// 往一条主题上发，没连上就悄悄跳过，和原版链路那边一个态度
static bool mqtt_send(const char *topic, const char *payload, bool retained) {
    bool ok;

    if (!s_mqtt_connected || topic == NULL || topic[0] == '\0' || payload == NULL) {
        return false;
    }

    // PubSubClient 只发得了 QoS0，原版那几条 QoS1 的降下来，retain 照样对上
    ok = s_mqtt.publish(topic, payload, retained);

    if (ok) {
        Serial.printf("mqtt: 发 %s %u 字节%s\n", topic, (unsigned)strlen(payload),
                      retained ? "（retain）" : "");
    } else {
        // 多半是报文比收发缓冲还长，或者刚掉线
        Serial.printf("mqtt: 发 %s 没成功（%u 字节），报文太长或者链路刚断\n",
                      topic, (unsigned)strlen(payload));
    }
    return ok;
}

// 上报一遍设备状态：设备快照后面补上自动模式、信号、地址、开机时长
void net_mqtt_publish_state(void) {
    char *snap;
    char *out;
    int n;
    int m;

    if (!s_mqtt_connected) {
        return;
    }

    // 报文不小，走堆，别把主循环那点栈吃掉
    snap = (char *)malloc(NET_MQTT_SNAP_LEN);
    out = (char *)malloc(NET_MQTT_STATE_LEN);
    if (snap == NULL || out == NULL) {
        Serial.printf("mqtt: 状态报文没内存了，本条不发\n");
        free(snap);
        free(out);
        return;
    }

    n = device_snapshot_json(snap, NET_MQTT_SNAP_LEN);
    if (n <= 0 || n >= (int)NET_MQTT_SNAP_LEN || snap[n - 1] != '}') {
        Serial.printf("mqtt: 设备快照不对（ret=%d），本条不发\n", n);
        free(snap);
        free(out);
        return;
    }
    // 把结尾那个花括号让开，后面还要往里补几项
    snap[n - 1] = '\0';

    // 字段名和原版 mqtt_publish_state 一样，就是 auto / rssi / ip / uptime 这四项
    m = snprintf(out, NET_MQTT_STATE_LEN,
                 "%s%s\"auto\":%s,\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu}",
                 snap,
                 // 空表就不用加逗号了
                 (n > 2) ? "," : "",
                 automation_is_enabled() ? "true" : "false",
                 (int)WiFi.RSSI(),
                 net_mqtt_ip_str(),
                 (unsigned long)(millis() / 1000UL));
    free(snap);

    if (m <= 0 || m >= (int)NET_MQTT_STATE_LEN) {
        // 太长的报文直接丢，别把缓冲写穿
        Serial.printf("mqtt: 状态报文太长（%d 字节），本条不发\n", m);
        free(out);
        return;
    }

    mqtt_send(s_topic_state, out, true);
    free(out);
}

// 上报一遍测量数据
void net_mqtt_publish_sensor(void) {
    const sensor_data_t *d;
    char buf[NET_MQTT_SENSOR_LEN];
    int n;

    if (!s_mqtt_connected) {
        return;
    }

    d = sensor_get_last();
    if (d == NULL) {
        return;
    }

    // 字段名和原版 mqtt_publish_sensor 对齐；板子上只有光敏电阻，
    // 原版报的 lux 和 light_is_bh1750 这版没数据，那两项就不发了，
    // 手机端缺字段会自己容错（蓝牙那边的报文也是这么处理的）
    n = snprintf(buf, sizeof(buf),
                 "{\"temp\":%.2f,\"humi\":%.2f,\"temp_valid\":%s,"
                 "\"light_pct\":%.1f,\"rain\":%.1f,\"rain_detected\":%s}",
                 (double)d->temperature, (double)d->humidity,
                 d->valid_temp ? "true" : "false",
                 (double)d->light_pct, (double)d->rain_pct,
                 d->rain_detected ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(buf)) {
        Serial.printf("mqtt: 传感器报文装不下（%d 字节），本条不发\n", n);
        return;
    }

    mqtt_send(s_topic_sensor, buf, false);
}

// 上报一遍阈值设置
void net_mqtt_publish_config(void) {
    char buf[NET_MQTT_CFG_LEN];
    int n;

    if (!s_mqtt_connected) {
        return;
    }

    n = automation_cfg_json(buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) {
        Serial.printf("mqtt: 阈值报文拼不出来（ret=%d），本条不发\n", n);
        return;
    }
    // 末尾补个结尾符，后头要拿去比字符串
    buf[n] = '\0';

    // 留个底，认出自己发出去的那份
    snprintf(s_last_pub_config, sizeof(s_last_pub_config), "%s", buf);

    mqtt_send(s_topic_config, buf, true);
    Serial.printf("mqtt: 阈值上报 %s\n", buf);
}

// 报一次本地事件
void net_mqtt_publish_event(const char *event) {
    char buf[NET_MQTT_EVENT_LEN];
    int n;

    if (!s_mqtt_connected) {
        return;
    }

    // 事件名都是自己人拼的短名字，没有引号反斜杠，不做转义
    n = snprintf(buf, sizeof(buf), "{\"event\":\"%s\"}", (event != NULL) ? event : "");
    if (n <= 0 || n >= (int)sizeof(buf)) {
        Serial.printf("mqtt: 事件报文太长（%s），本条不发\n", (event != NULL) ? event : "");
        return;
    }

    mqtt_send(s_topic_event, buf, false);
}

// 回一条执行结果，形状和原版 mqtt_publish_ack 一样
void net_mqtt_publish_ack(const char *action, bool ok, const char *msg) {
    char buf[NET_MQTT_ACK_LEN];
    int n;

    if (!s_mqtt_connected) {
        return;
    }

    // 动作名和设备名都是表里写死的短名字，带不进引号，不做转义
    n = snprintf(buf, sizeof(buf),
                 "{\"action\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
                 (action != NULL) ? action : "",
                 ok ? "true" : "false",
                 (msg != NULL) ? msg : "");
    if (n <= 0 || n >= (int)sizeof(buf)) {
        Serial.printf("mqtt: 回执报文太长（%s），本条不发\n", (action != NULL) ? action : "");
        return;
    }

    mqtt_send(s_topic_ack, buf, false);
}

// ----------------------------------------------------------------------
// 收进来
// ----------------------------------------------------------------------

// 把一层平铺的键值挑出来交给联动
// 原版拿 cJSON 逐项走，这版手写一层：数字和 true/false 都收，别的跳过。
// 返回 false 就是 JSON 压根不认
static bool config_apply_json(const char *json, int *applied, int *rejected) {
    const char *p = json;
    char key[32];

    if (json == NULL || applied == NULL || rejected == NULL) {
        return false;
    }

    p = skip_ws(p);
    if (*p != '{') {
        return false;
    }
    p++;

    for (;;) {
        size_t klen = 0;
        bool key_ok = true;
        bool numeric = false;
        double value = 0.0;

        p = skip_ws(p);
        if (*p == '}') {
            break;
        }
        if (*p != '"') {
            // 键必须是带引号的名字
            return false;
        }

        // 把键名抠出来
        p++;
        while (*p != '\0' && *p != '"') {
            if (klen < (sizeof(key) - 1)) {
                key[klen] = *p;
            } else {
                // 比格子还长，肯定不是联动认的那几条线
                key_ok = false;
            }
            klen++;
            p++;
        }
        if (*p != '"') {
            return false;
        }
        p++;
        key[(klen < (sizeof(key) - 1)) ? klen : (sizeof(key) - 1)] = '\0';

        p = skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p++;
        p = skip_ws(p);

        // 值：数字和 true/false 都算数，别的整段跳过去，和原版的 cJSON 分支一样
        if (strncmp(p, "true", 4) == 0) {
            value = 1.0;
            numeric = true;
            p += 4;
        } else if (strncmp(p, "false", 5) == 0) {
            value = 0.0;
            numeric = true;
            p += 5;
        } else if (*p == '-' || *p == '+' || (*p >= '0' && *p <= '9')) {
            char *end = NULL;

            value = strtod(p, &end);
            numeric = (end != NULL) && (end != p);
            p = (end != NULL) ? end : p;
        } else {
            p = skip_json_value(p);
        }

        if (!key_ok) {
            (*rejected)++;
            Serial.printf("mqtt: 阈值名字太长（%u 字节），跳过\n", (unsigned)klen);
        } else if (!numeric) {
            (*rejected)++;
            Serial.printf("mqtt: 阈值 %s 不是数字，跳过\n", key);
        } else if (automation_set_threshold(key, (float)value)) {
            (*applied)++;
            Serial.printf("mqtt: 阈值 %s = %g\n", key, (double)value);
        } else {
            (*rejected)++;
            Serial.printf("mqtt: 阈值 %s = %g 没收（越界或者联动不认这条）\n",
                          key, (double)value);
        }

        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            break;
        }
        return false;
    }

    return true;
}

// 收到阈值：一条条改，改完存盘，再回一份完整的让对方确认
static void handle_config(const char *payload, int len) {
    int applied = 0;
    int rejected = 0;

    // 原版这边也是按 256 卡长度，超了直接不要
    if (payload == NULL || len <= 0 || len >= NET_MQTT_CFG_RX_LEN) {
        Serial.printf("mqtt: 阈值报文空的或者超过 %d 字节，已忽略\n", NET_MQTT_CFG_RX_LEN);
        return;
    }

    // 认出自己刚发出去的那份，别自己改自己
    if (s_last_pub_config[0] != '\0' && strcmp(payload, s_last_pub_config) == 0) {
        Serial.printf("mqtt: 收到自己发的阈值，跳过\n");
        return;
    }

    if (!config_apply_json(payload, &applied, &rejected)) {
        Serial.printf("mqtt: 阈值不是合法 JSON：%s\n", payload);
        net_mqtt_publish_ack("config", false, "bad json");
        return;
    }

    // 顺序照原版：先存盘，再回一份配置，最后回执
    automation_save();
    net_mqtt_publish_config();
    net_mqtt_publish_ack("config", rejected == 0, (rejected == 0) ? "applied" : "partly applied");
    Serial.printf("mqtt: 阈值处理完 %d 条生效，%d 条没收\n", applied, rejected);
}

// 看主题分给谁处理，原版 handle_message 一个路子
static void handle_message(const char *topic, const char *payload, int len) {
    if (topic == NULL || topic[0] == '\0') {
        Serial.printf("mqtt: 报文没带主题，已忽略\n");
        return;
    }

    if (topic_is_cmd(topic)) {
        char action[24] = { 0 };
        char dev[32] = { 0 };
        const char *detail;
        bool ok;

        // 回执里要带动作名，先从报文里抠出来，抠不到写个大概的名字
        if (!json_get_str(payload, CMD_KEY_ACTION, action, sizeof(action))) {
            snprintf(action, sizeof(action), "%s", "cmd");
        }

        // 执行归 app_cmd，和蓝牙共用一套解析，来源标成手机走网络
        ok = app_cmd_handle_json(payload, len, SRC_MQTT);

        // 原版每条命令都回一句，这版命令那边只给成没成，回执归链路这边发；
        // 成了就把设备名丢进 detail，和原版一个写法
        detail = ok ? "done" : "rejected";
        if (ok && json_get_str(payload, CMD_KEY_DEV, dev, sizeof(dev))) {
            detail = dev;
        }
        net_mqtt_publish_ack(action, ok, detail);

        // 切自动模式的话，原版跟着再报一份阈值，这儿照办
        if (ok && strcmp(action, CMD_ACTION_AUTO) == 0) {
            net_mqtt_publish_config();
        }
        return;
    }

    if (topic_endswith(topic, MQTT_SUFFIX_CONFIG)) {
        handle_config(payload, len);
        return;
    }

    if (topic_endswith(topic, MQTT_SUFFIX_GET)) {
        // 原版这儿只回 state + sensor，这版按咱们的约定多回一份 config
        Serial.printf("mqtt: 收到 get，回 state + sensor + config\n");
        net_mqtt_publish_state();
        net_mqtt_publish_sensor();
        net_mqtt_publish_config();
        return;
    }

    Serial.printf("mqtt: 主题 %s 没人管，已忽略\n", topic);
}

// PubSubClient 的回调：它给的 payload 不带结尾零，先抄一份再解析
static void net_mqtt_callback(char *topic, uint8_t *payload, unsigned int len) {
    if (len >= sizeof(s_rx)) {
        // 太长的报文直接丢，别把缓冲写穿
        Serial.printf("mqtt: 报文 %u 字节超过缓冲 %u，已丢\n",
                      len, (unsigned)sizeof(s_rx));
        return;
    }

    if (payload != NULL && len > 0) {
        memcpy(s_rx, payload, (size_t)len);
    }
    s_rx[len] = '\0';

    handle_message(topic, s_rx, (int)len);
}

// ----------------------------------------------------------------------
// 链路状态：外面就靠这几个口子问
// ----------------------------------------------------------------------

// 拿到 IP 才算真的通，光连上 AP 不算
static bool wifi_link_up(void) {
    return WiFi.status() == WL_CONNECTED;
}

bool net_mqtt_wifi_is_connected(void) {
    return wifi_link_up();
}

bool net_mqtt_is_connected(void) {
    return s_mqtt_connected;
}

// WiFi 信号强度，没连上给 0；只读，什么都不动
int net_mqtt_rssi(void) {
    if (!wifi_link_up()) {
        return 0;
    }
    return (int)WiFi.RSSI();
}

// 查到本机 IP 给外面看，没连上给 0.0.0.0
const char *net_mqtt_ip_str(void) {
    if (!wifi_link_up()) {
        return "0.0.0.0";
    }

    // 地址每次现取，省得和事件那边抢一个字
    snprintf(s_ip_str, sizeof(s_ip_str), "%s", WiFi.localIP().toString().c_str());
    return s_ip_str;
}

// 拼主题和客户端名都用它，原版管这个叫 uid；
// 没连网也算得出来，蓝牙那边拼设备名也来问这一个口
const char *net_mqtt_device_id(void) {
    return uid_ensure();
}

// ----------------------------------------------------------------------
// 连接管理：全在 poll 里，绝不死等
// ----------------------------------------------------------------------

// 掉线原因只有事件里有，事件任务里只记一个数，日志留给主循环打
static void net_wifi_disconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)event;
    s_disc_reason = info.wifi_sta_disconnected.reason;
}

// WiFi 这摊：认状态变化、掉线了隔几秒再试
static void wifi_poll(uint32_t now_ms) {
    bool up = wifi_link_up();

    if (up != s_wifi_up) {
        s_wifi_up = up;
        if (up) {
            s_wifi_retry = 0;
            Serial.printf("wifi: 连上了，ip=%s\n", net_mqtt_ip_str());
            // 网刚回来，MQTT 那边别干等，下一圈就去试
            s_mqtt_backoff_ms = NET_MQTT_RETRY_MIN_MS;
            s_mqtt_try_ms = now_ms - NET_MQTT_RETRY_MIN_MS;
        } else {
            Serial.printf("wifi: 掉线了 (reason=%u)，后台接着连\n", (unsigned)s_disc_reason);
        }
    }

    if (up) {
        return;
    }

    // 节流：五秒才试一次，不每圈都喊，也不刷屏
    if ((now_ms - s_wifi_try_ms) < NET_MQTT_WIFI_RETRY_MS) {
        return;
    }
    s_wifi_try_ms = now_ms;

    // 重连一律走 begin：WiFi.reconnect() 第一步就 disconnect，
    // 没连上的时候那步直接失败，压根不会发起连接，等于白喊
    WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);

    s_wifi_retry++;
    Serial.printf("wifi: 第 %d 次重连 \"%s\" ...\n", s_wifi_retry, APP_WIFI_SSID);
}

// 连上了：订下行、报在线、先报一遍现状，和原版 MQTT_EVENT_CONNECTED 那段一样
static void mqtt_on_connected(void) {
    s_mqtt_connected = true;
    s_mqtt_backoff_ms = NET_MQTT_RETRY_MIN_MS;
    Serial.printf("mqtt: 连上 %s:%d (client_id=%s)\n",
                  APP_MQTT_BROKER, (int)APP_MQTT_PORT, s_client_id);

    // 订下三条下行，原版订哪几条这儿就订哪几条
    if (!s_mqtt.subscribe(s_topic_cmd_wild, 1)) {
        Serial.printf("mqtt: 订 %s 没成\n", s_topic_cmd_wild);
    }
    if (!s_mqtt.subscribe(s_topic_config, 1)) {
        Serial.printf("mqtt: 订 %s 没成\n", s_topic_config);
    }
    if (!s_mqtt.subscribe(s_topic_get, 1)) {
        Serial.printf("mqtt: 订 %s 没成\n", s_topic_get);
    }

    // 报一声我在线，retain 住，手机一上来就知道板子在
    mqtt_send(s_topic_availability, MQTT_PAYLOAD_ONLINE, true);

    // 连上先报一遍
    net_mqtt_publish_state();
    net_mqtt_publish_sensor();
}

// 连不上就退避：等的时间翻倍，封顶一分钟
static void mqtt_backoff_next(const char *why) {
    if (s_mqtt_backoff_ms < NET_MQTT_RETRY_MAX_MS) {
        s_mqtt_backoff_ms *= 2;
        if (s_mqtt_backoff_ms > NET_MQTT_RETRY_MAX_MS) {
            s_mqtt_backoff_ms = NET_MQTT_RETRY_MAX_MS;
        }
    }

    Serial.printf("mqtt: 连 %s:%d 不成（%s），%u 秒后再试\n",
                  APP_MQTT_BROKER, (int)APP_MQTT_PORT, why,
                  (unsigned)(s_mqtt_backoff_ms / 1000));
}

// 连一次 broker，连不上就退避，绝不在这儿死等
static bool mqtt_try_connect(void) {
    const char *user = (APP_MQTT_USER[0] != '\0') ? APP_MQTT_USER : NULL;
    const char *pass = (APP_MQTT_PASSWORD[0] != '\0') ? APP_MQTT_PASSWORD : NULL;
    char why[32];
    bool ok;

    // 先用短超时把 TCP 捅开：PubSubClient 自己连默认要等十几秒，主循环受不了
    if (!s_net_client.connected()) {
        if (s_net_client.connect(APP_MQTT_BROKER, APP_MQTT_PORT, NET_MQTT_TCP_TIMEOUT_MS) != 1) {
            mqtt_backoff_next("TCP 没通");
            return false;
        }
    }

    // 遗嘱挂上：真掉线时 broker 替我们发 offline，主题和原版一样
    ok = s_mqtt.connect(s_client_id, user, pass,
                        s_topic_availability, 1, true, MQTT_PAYLOAD_OFFLINE);
    if (!ok) {
        // 这条 socket 已经废了，放掉，下次重开
        s_net_client.stop();
        snprintf(why, sizeof(why), "state=%d", s_mqtt.state());
        mqtt_backoff_next(why);
        return false;
    }

    mqtt_on_connected();
    return true;
}

// MQTT 这摊：掉线喊一声、节流重连、连上了就收发
static void mqtt_poll(uint32_t now_ms) {
    // 连着呢：心跳和收报文都靠 loop
    if (s_mqtt.connected()) {
        s_mqtt.loop();
        return;
    }

    if (s_mqtt_connected) {
        s_mqtt_connected = false;
        // 掉线了自己会重连，别在这儿死等
        s_mqtt_backoff_ms = NET_MQTT_RETRY_MIN_MS;
        Serial.printf("mqtt: 掉线了 (state=%d)，等会儿自己重连\n", s_mqtt.state());
    }

    // 节流没过就先歇着，别每圈都去撞 broker
    if ((now_ms - s_mqtt_try_ms) < s_mqtt_backoff_ms) {
        return;
    }
    s_mqtt_try_ms = now_ms;

    mqtt_try_connect();
}

// 主循环喊这个，重连和收报文都在里面
void net_mqtt_poll(void) {
    uint32_t now_ms;

    if (!s_inited) {
        return;
    }

    // 账号没填就安静离线，别一遍遍刷屏
    if (s_wifi_offline) {
        return;
    }

    now_ms = millis();
    wifi_poll(now_ms);

    if (!wifi_link_up()) {
        // 网都不通就别去连 broker，白等一趟超时
        if (s_mqtt_connected) {
            s_mqtt_connected = false;
            // 这条已经死了，收掉，等网回来重新开
            s_mqtt.disconnect();
            Serial.printf("mqtt: 网断了，连接收掉，等网回来再连\n");
        }
        return;
    }

    mqtt_poll(now_ms);
}

// 把 WiFi 和 MQTT 都准备好，WiFi 没填账号就安静地离线跑
bool net_mqtt_init(void) {
    if (s_inited) {
        Serial.printf("mqtt: 已经弄过了，跳过\n");
        return true;
    }

    // 设备号和主题先拼好，离线也用得上，屏幕上和蓝牙那边都要显示本机号
    const char *uid = uid_ensure();

    if (!build_all_topics()) {
        return false;
    }
    // 客户端名必须唯一，原版是 esp32sh-<号>
    snprintf(s_client_id, sizeof(s_client_id), "esp32sh-%s", uid);

    // 账号还是占位符：不连、不死等，安静离线跑
    if (APP_WIFI_SSID[0] == '\0' || strcmp(APP_WIFI_SSID, "YOUR_WIFI_SSID") == 0 ||
        strcmp(APP_WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") == 0) {
        s_wifi_offline = true;
        s_inited = true;
        Serial.printf("wifi: WiFi 还没配置：请编辑 arduino/esp32_smart_home/app_config.h "
                      "填上 APP_WIFI_SSID 与 APP_WIFI_PASSWORD\n");
        Serial.printf("wifi: 先离线跑，蓝牙和本地按键照常用，设备号 %s\n", uid);
        return true;
    }

    WiFi.mode(WIFI_STA);
    // 核心自己也带自动重连，poll 里那层节流重连是兜底
    WiFi.setAutoReconnect(true);
    // 掉线原因只有事件里给，事件任务里只记一个数
    WiFi.onEvent(net_wifi_disconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    s_mqtt.setServer(APP_MQTT_BROKER, APP_MQTT_PORT);
    s_mqtt.setCallback(net_mqtt_callback);
    s_mqtt.setKeepAlive(APP_MQTT_KEEPALIVE_S);
    // 等 CONNACK 别超过几秒，主循环还等着转呢
    s_mqtt.setSocketTimeout(NET_MQTT_SOCKET_TIMEOUT_S);
    // 默认 256 字节装不下状态报文，先把收发缓冲撑开
    if (!s_mqtt.setBufferSize(NET_MQTT_BUF_LEN)) {
        Serial.printf("mqtt: 收发缓冲开到 %u 没成，长报文可能发不出去\n",
                      (unsigned)NET_MQTT_BUF_LEN);
    }

    s_inited = true;
    s_wifi_up = false;
    s_wifi_retry = 0;
    s_mqtt_connected = false;
    s_mqtt_backoff_ms = NET_MQTT_RETRY_MIN_MS;
    s_wifi_try_ms = millis();
    s_mqtt_try_ms = millis();

    Serial.printf("mqtt: 参数备好了 broker=%s:%d client_id=%s\n",
                  APP_MQTT_BROKER, (int)APP_MQTT_PORT, s_client_id);
    Serial.printf("mqtt: 主题 state=%s cmd=%s config=%s get=%s\n",
                  s_topic_state, s_topic_cmd, s_topic_config, s_topic_get);

    // 起第一轮：连不上就交给 poll 慢慢来，这儿不死等（原版是等十秒）
    WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);
    Serial.printf("wifi: 开始连 \"%s\" ...\n", APP_WIFI_SSID);

    return true;
}

#else  // APP_MQTT_ENABLE == 0

// 没装 PubSubClient 库、或者 app_config.h 里把开关关了：全空着，保证还能编过
// 这一支里没有 include PubSubClient.h，所以库没装也照样编

bool net_mqtt_init(void) {
    Serial.printf("mqtt: 未编译进固件（APP_MQTT_ENABLE=0），上云不可用；"
                  "蓝牙和本地控制不受影响\n");
    return false;
}

void net_mqtt_poll(void) {
    // 没联网这摊，什么都不用维护
}

bool net_mqtt_wifi_is_connected(void) {
    return false;
}

bool net_mqtt_is_connected(void) {
    return false;
}

int net_mqtt_rssi(void) {
    // 没联网就没信号强度可报
    return 0;
}

void net_mqtt_publish_state(void) {
    // 没链路，直接扔
}

void net_mqtt_publish_sensor(void) {
    // 没链路，直接扔
}

void net_mqtt_publish_config(void) {
    // 没链路，直接扔
}

void net_mqtt_publish_event(const char *event) {
    (void)event;
}

void net_mqtt_publish_ack(const char *action, bool ok, const char *msg) {
    (void)action;
    (void)ok;
    (void)msg;
}

const char *net_mqtt_ip_str(void) {
    return "0.0.0.0";
}

const char *net_mqtt_device_id(void) {
    // 没联网也得给蓝牙那边一个号，还是 MAC 后三位那一套，
    // 和开着联网时算出来的一个样
    return uid_ensure();
}

#endif  // APP_MQTT_ENABLE
