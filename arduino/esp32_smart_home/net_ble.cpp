// ======================================================================
// 蓝牙：手机 App 直连（Arduino 版）
//
// 对应 ESP-IDF 工程的 components/App/ble_app.c，服务号、特征号和报文一个没改，
// 手机 App 那边不用动。
//
// 依赖 NimBLE-Arduino 库（NimBLEDevice.h），1.x 和 2.x 都能编：
// 回调签名两代不一样，按库自带的版本宏分两套写，只覆写那个版本确实有的口，
// 没把握的（比如 CCCD 订阅回调）宁可绕开，也不写一个"看着像重载、其实不生效"的函数。
//
// 还有一件事跟原工程对齐：蓝牙回调是在蓝牙任务里跑的，设备状态表、自动化、
// 屏幕都是主循环在用，两边同时改会打架。所以回调里只把报文塞进一个定长队列，
// 真正的解析和执行留给 net_ble_poll()，它是在 loop() 里被喊的。
//
// app_config.h 里 APP_BLE_ENABLE 为 0（或者没装 NimBLE 库被自动改成 0）时，
// 下面整块都不参与编译，连 NimBLEDevice.h 都不 include，所以没装库也能编过。
// ======================================================================

#include "app_config.h"
#include "net_ble.h"

// Serial 两边都要用（关掉蓝牙那一支也要打日志），所以这几个头放在开关外头
// 只有 NimBLEDevice.h 必须留在开关里面：没装库的时候不能 include 它
#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if APP_BLE_ENABLE

#include <NimBLEDevice.h>

#include "app_cmd.h"
#include "automation.h"
#include "device_model.h"
#include "sensor.h"
// 状态报文里要带本机 IP，就这一样从联网那边拿，拿不到给 0.0.0.0
#include "net_mqtt.h"

// ---- 协议：UUID 和原版 ble_app.c 一个字都不差 ----
// 原版是 BLE_UUID128_INIT 小端写的，翻过来就是手机 Contract.kt 里的标准写法
// 服务号 a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d
#define BLE_SVC_UUID     "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"
// 手机写进来（RX 特征）
#define BLE_RX_UUID      "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e"
// 板子推出去（TX 特征）
#define BLE_TX_UUID      "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f"

// ---- 下行帧头：手机发来的第一字节 ----
// 控制命令
#define BLE_DOWN_CMD     0x01
// 阈值配置
#define BLE_DOWN_CONFIG  0x02
// 要一份现状
#define BLE_DOWN_GET     0x03

// ---- 上行帧头：板子推上去的第一字节 ----
// 对齐 app_link.h 里的 app_msg_type_t，也对着手机 Contract.kt 的 Proto.Up
// 设备状态
#define BLE_UP_STATE     0x01
// 传感器读数
#define BLE_UP_SENSOR    0x02
// 阈值配置
#define BLE_UP_CONFIG    0x05

// 通知和指示多占三字节（ATT 头的 opcode + handle）
#define BLE_ATT_OVERHEAD 3

// 广播名最长二十九字节，超了广播包装不下
#define BLE_NAME_MAX_LEN 29

// ---- 缓冲区：都给得宽松点，这点内存换不截断 ----
// 收手机那一帧，原版 BLE_RX_BUF_LEN 就是这么大
#define BLE_RX_BUF_LEN     512
// 拼状态报文，设备快照加上几个补充字段装得下
#define BLE_STATE_BUF_LEN  1024
// 广播名缓冲区：二十九字节名字加结尾，再留几个字节富余
#define BLE_NAME_BUF_LEN   40

// ---- 命令队列：回调那边只塞进来，主循环那边取出去 ----
// 手机一帧最长这么大，超了的帧塞不进队列，只能丢
#define BLE_QUEUE_PAYLOAD 256
// 排四条够了，手机点一下不会连发一堆
#define BLE_QUEUE_SLOTS   4

// ----------------------------------------------------------------------
// 版本差异就集中在下面这几行，全是编译期挑好的，跑起来没有分支
//
// NimBLE-Arduino 2.x 的 NimBLEDevice.h 会带一个 NimBLECppVersion.h，
// 里头定义了 NIMBLE_CPP_VERSION_MAJOR；1.x 没有这个头，认不出这个宏。
// 回调签名在 2.x 那代改过一次（都改成传 NimBLEConnInfo&），
// 所以按这个宏分两套写。
// ----------------------------------------------------------------------
#if defined(NIMBLE_CPP_VERSION_MAJOR) && (NIMBLE_CPP_VERSION_MAJOR >= 2)
#define BLE_NIMBLE_V2 1
#else
#define BLE_NIMBLE_V2 0
#endif

// 对端 MTU 的两种读法
// 新版按连接号读，老版按广播里那个号读（0xFFFF 就是当前这条）
#if BLE_NIMBLE_V2
#define BLE_PEER_MTU(h) (NimBLEDevice::getServer() ? NimBLEDevice::getServer()->getPeerMTU((h)) : 0)
#else
#define BLE_PEER_MTU(h) (NimBLEDevice::getServer() ? NimBLEDevice::getServer()->getPeerMTU(0xFFFF) : 0)
#endif

// 扫描响应开关也改过名：老版叫 setScanResponse，新版叫 enableScanResponse
#if BLE_NIMBLE_V2
#define BLE_ADV_SCAN_RSP_ON() NimBLEDevice::getAdvertising()->enableScanResponse(true)
#else
#define BLE_ADV_SCAN_RSP_ON() NimBLEDevice::getAdvertising()->setScanResponse(true)
#endif

// 有没有人订阅，两边都这么问
// 新版 getSubscribedCount() 返回 uint8_t，老版返回 size_t，零都是没人听
#define BLE_TX_SUBSCRIBED(c) ((c) != NULL && (c)->getSubscribedCount() > 0)

// 两边都认的初始化写法
// 服务器这边不去抢着要大 MTU：手机那边会自己 requestMtu(517)，谈成多少我们照单全收，
// 发之前按谈好的值判一下就发，这样哪个库版本都不会因为 API 不一样编不过
#define BLE_INIT_DEVICE(name) NimBLEDevice::init((name))
// 广播的启动口两个版本一模一样
// 传对象进来是先判过空指针了，启动就是打一下的事，不阻塞
#define BLE_START_ADV(adv)    ((adv) != NULL && (adv)->start())

// ----------------------------------------------------------------------
// 模块状态
// ----------------------------------------------------------------------

// 初始化过就不再来一遍
static bool s_inited = false;
// 手机连着没，回调里改
static bool s_connected = false;
// 现在正在广播没，断开后要重开
static bool s_advertising = false;
// 手机那一路连接号，读 MTU 用
static uint16_t s_conn_handle = 0xFFFF;

// 广播名：APP_BLE_NAME 加个横杠再加板子号，像 SmartHome-4d4a64
// 存到静态数组里，广播和 GAP 都只是借这个指针用，得一直活着
static char s_dev_name[BLE_NAME_BUF_LEN] = { 0 };

// 两个特征先存着，建服务的时候回填
static NimBLECharacteristic *s_rx_chr = NULL;
static NimBLECharacteristic *s_tx_chr = NULL;

// 切帧的地方，放全局省栈
static uint8_t s_chunk[BLE_RX_BUF_LEN + 2];
// 拼报文的地方
static char s_tx_buf[BLE_STATE_BUF_LEN];

// ---- 命令队列本体：定长数组，不 malloc ----
// 每条一个类型码加一段报文，凑成一个定长结构好算下标
typedef struct {
    uint8_t kind;
    uint16_t len;
    uint8_t payload[BLE_QUEUE_PAYLOAD];
} ble_queue_item_t;

static ble_queue_item_t s_queue[BLE_QUEUE_SLOTS];
// 写的位置只有蓝牙任务动，读的位置只有主循环动，各管一个，不用加锁
static volatile uint8_t s_q_head = 0;
static volatile uint8_t s_q_tail = 0;

// ----------------------------------------------------------------------
// 小工具
// ----------------------------------------------------------------------

// 上行帧类型的名字，日志里照原版那么写
static const char *ble_up_type_name(uint8_t type) {
    switch (type) {
    case BLE_UP_STATE:
        return "state";
    case BLE_UP_SENSOR:
        return "sensor";
    case BLE_UP_CONFIG:
        return "config";
    default:
        return "unknown";
    }
}

// 拼广播名：APP_BLE_NAME + '-' + 板子号，跟原版 ble_build_device_name 一个做法
// 板子号留空时 net_mqtt_device_id() 会用芯片 MAC 后三位自己拼一个
// 名字太长就截短并告警，别让广播包塞不下
static void ble_build_device_name(void) {
    const char *uid = net_mqtt_device_id();
    int n;

    if (uid == NULL || uid[0] == '\0') {
        // 联网整个关掉时那边给的是空串，名字至少得是个像样的东西
        uid = "esp32";
    }

    n = snprintf(s_dev_name, sizeof(s_dev_name), "%s-%s", APP_BLE_NAME, uid);

    if (n < 0 || n >= (int)sizeof(s_dev_name)) {
        // snprintf 会自己收尾，这儿只是报一声
        Serial.printf("BLE: 广播名太长，已截成 \"%s\"（最多 %u 字节）\n",
                      s_dev_name, (unsigned)sizeof(s_dev_name) - 1);
        return;
    }

    if (n > BLE_NAME_MAX_LEN) {
        // 传统广播的扫描响应只装得下二十九字节的名字，长了广播包里就没有全名
        Serial.printf("BLE: 广播名 \"%s\" 有 %d 字节，超过 %d 字节，"
                      "手机可能只看到一个短名，建议把 APP_BLE_NAME 改短点\n",
                      s_dev_name, n, BLE_NAME_MAX_LEN);
    }
}

// 读对端谈好的 MTU
// 太小的按 23 兜底，太大的按 512 封顶
static uint16_t ble_peer_mtu(void) {
    uint16_t mtu = (uint16_t)BLE_PEER_MTU(s_conn_handle);

    if (mtu < 23) {
        mtu = 23;
    } else if (mtu > 512) {
        mtu = 512;
    }
    return mtu;
}

// 队列空没空
static bool ble_queue_empty(void) {
    return (s_q_head == s_q_tail);
}

// 塞一条进队列，只在蓝牙回调里调
// 满了就丢最新这条，不覆盖还没处理的，更不阻塞
static bool ble_queue_push(uint8_t kind, const uint8_t *payload, uint16_t len) {
    uint8_t next;

    if (payload == NULL || len == 0) {
        return false;
    }
    // 太长的帧队列装不下，只能丢，别截半截给主循环
    if (len > BLE_QUEUE_PAYLOAD) {
        Serial.printf("BLE: 帧 %u 字节超过队列单条上限 %u 字节，丢掉一条\n",
                      (unsigned)len, (unsigned)BLE_QUEUE_PAYLOAD);
        return false;
    }

    next = (uint8_t)((s_q_head + 1) % BLE_QUEUE_SLOTS);
    if (next == s_q_tail) {
        Serial.printf("BLE: 命令队列满了，丢掉一条\n");
        return false;
    }

    s_queue[s_q_head].kind = kind;
    s_queue[s_q_head].len  = len;
    memcpy(s_queue[s_q_head].payload, payload, (size_t)len);
    // 内容都填好了再挪头，主循环那边才不会读到半条
    s_q_head = next;

    return true;
}

// 从队列取一条出来，只在主循环里调
static bool ble_queue_pop(uint8_t *kind, uint8_t *payload, uint16_t *len) {
    if (ble_queue_empty()) {
        return false;
    }

    *kind = s_queue[s_q_tail].kind;
    *len  = s_queue[s_q_tail].len;
    memcpy(payload, s_queue[s_q_tail].payload, (size_t)s_queue[s_q_tail].len);
    // 拷完才挪尾
    s_q_tail = (uint8_t)((s_q_tail + 1) % BLE_QUEUE_SLOTS);

    return true;
}

// 开广播
// 名字放扫描响应里，主包留给服务号，跟原版一个套路
static void ble_start_advertising(void) {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

    if (adv == NULL) {
        Serial.printf("BLE: 拿不到广播对象，放弃开广播\n");
        return;
    }

    if (s_advertising) {
        // 已经在播了就别打扰协议栈
        return;
    }

    adv->addServiceUUID(BLE_SVC_UUID);
    // 广播名带板子号，和原版一样，手机扫描列表里显示的就是它
    adv->setName(s_dev_name);
    BLE_ADV_SCAN_RSP_ON();

    if (BLE_START_ADV(adv)) {
        s_advertising = true;
        Serial.printf("BLE: 广播已开始，名 \"%s\"（在扫描响应里），Service UUID 在主包里\n",
                      s_dev_name);
    } else {
        Serial.printf("BLE: 广播没开起来，等 poll 里再试\n");
    }
}

// 手机连上了
// 这个在蓝牙任务里跑，只记状态，别碰设备表
// 连接号从回调里带进来：问服务器的口各个版本长得不一样，干脆不用
// MTU 这儿先不报，刚连上多半还没谈，等发第一包的时候再打
static void ble_handle_connect(uint16_t conn_handle) {
    s_connected = true;
    s_advertising = false;
    s_conn_handle = conn_handle;

    Serial.printf("BLE: 手机已连接（handle=%u），等手机来要数据\n",
                  (unsigned)s_conn_handle);
}

// 手机断开了
// 清干净再重开广播等它回来
static void ble_handle_disconnect(void) {
    Serial.printf("BLE: 手机已断开，重新开广播\n");

    s_connected = false;
    s_conn_handle = 0xFFFF;
    ble_start_advertising();
}

// 往手机发一整帧
// 装得下就一条发出去，装不下就不发
// 手机那边是一条通知一帧、不做跨通知拼包，切开发过去它只会记一句
// "未知类型码"，比不发还糟；所以跟原工程 ble_app.c 一个做法：
// 超了 MTU 就整条放弃，只打一句警告
static void ble_send_frame(uint8_t type, const char *json, int len) {
    int frame_len;
    uint16_t mtu;
    uint16_t payload;

    // 没人订阅就白算，先看有没有连上、有没有人听
    // 订阅状态直接问特征，别指望 CCCD 回调：那个口 1.4 和 1.5 长得不一样
    if (!s_connected || !BLE_TX_SUBSCRIBED(s_tx_chr)) {
        return;
    }
    if (s_tx_chr == NULL || json == NULL || len <= 0) {
        return;
    }

    // 头一字节是类型
    frame_len = len + 1;
    if (frame_len > (int)sizeof(s_chunk)) {
        // 内部缓冲先兜一道，免得把栈写穿
        Serial.printf("BLE: 帧 %d 字节超过内部缓冲 %u 字节，本条不上报\n",
                      frame_len, (unsigned)sizeof(s_chunk));
        return;
    }

    // 每次现问 MTU，不缓存：大 MTU 是手机连上后自己 requestMtu 谈出来的，随时会变
    mtu = ble_peer_mtu();
    payload = (mtu > BLE_ATT_OVERHEAD) ? (uint16_t)(mtu - BLE_ATT_OVERHEAD) : 20;

    // 装不下就整条不发
    // 截半截或者切开发出去，手机那边都只会看到一段认不出的东西
    if (frame_len > (int)payload) {
        Serial.printf("BLE: %s 帧 %d 字节 > 当前 MTU %u 的净载荷 %u 字节，本条不上报\n",
                      ble_up_type_name(type), frame_len,
                      (unsigned)mtu, (unsigned)payload);
        return;
    }

    s_chunk[0] = type;
    memcpy(s_chunk + 1, json, (size_t)len);

    s_tx_chr->setValue(s_chunk, (size_t)frame_len);

#if BLE_NIMBLE_V2
    // 新版 notify() 回一个 bool，能看出底层收没收下
    // 忙或者刚断开就会给 false，不阻塞主循环，下一条心跳自己会补上
    if (!s_tx_chr->notify()) {
        Serial.printf("BLE: notify(%s) 失败（多半是刚断开或底层拥塞）\n",
                      ble_up_type_name(type));
    }
#else
    // 老版 notify() 是 void，收没收下没法当场知道，发完看下一包的效果
    s_tx_chr->notify();
#endif
}

// 回一份实时传感器读数
// 字段名照原版 mqtt_publish_sensor，手机按这些名字解
static void ble_send_sensor_now(void) {
    const sensor_data_t *d = sensor_get_last();
    int n;

    if (d == NULL) {
        return;
    }

    // 板子上就一个光敏电阻，没有 lux 和 BH1750 那个标记，所以这两项不发，
    // 手机那边缺字段会自己容错
    n = snprintf(s_tx_buf, sizeof(s_tx_buf),
                 "{\"temp\":%.2f,\"humi\":%.2f,\"temp_valid\":%s,"
                 "\"light_pct\":%.1f,\"light_mv\":%d,"
                 "\"rain\":%.1f,\"rain_detected\":%s,\"ts\":%lu}",
                 (double)d->temperature, (double)d->humidity,
                 d->valid_temp ? "true" : "false",
                 (double)d->light_pct, d->light_mv,
                 (double)d->rain_pct, d->rain_detected ? "true" : "false",
                 (unsigned long)d->timestamp_ms);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(s_tx_buf)) {
        Serial.printf("BLE: 传感器报文 %d 字节装不下（缓冲 %u），本条不上报\n",
                      n, (unsigned)sizeof(s_tx_buf));
        return;
    }

    ble_send_frame(BLE_UP_SENSOR, s_tx_buf, n);
}

// 回一份阈值配置
static void ble_send_config_now(void) {
    int n = automation_cfg_json(s_tx_buf, sizeof(s_tx_buf));

    if (n <= 0 || n >= (int)sizeof(s_tx_buf)) {
        Serial.printf("BLE: automation_cfg_json 失败（ret=%d，缓冲 %u），本条不上报\n",
                      n, (unsigned)sizeof(s_tx_buf));
        return;
    }

    ble_send_frame(BLE_UP_CONFIG, s_tx_buf, n);
}

// 回一份设备状态
// 设备快照是一整段 JSON，末尾补上自动模式、信号、本机 IP、开机时长这几项，
// 字段和顺序都跟原版 mqtt_publish_state 一样
static void ble_send_state_now(void) {
    int n;
    char *end;

    n = device_snapshot_json(s_tx_buf, sizeof(s_tx_buf));
    if (n <= 0 || n >= (int)sizeof(s_tx_buf)) {
        Serial.printf("BLE: device_snapshot_json 失败（ret=%d，缓冲 %u），本条不上报\n",
                      n, (unsigned)sizeof(s_tx_buf));
        return;
    }

    // 找最后一个右花括号，从那儿插字段
    end = strrchr(s_tx_buf, '}');
    if (end == NULL) {
        Serial.printf("BLE: 设备快照不是完整 JSON，本条不上报\n");
        return;
    }
    *end = '\0';

    n = snprintf(end, (size_t)(sizeof(s_tx_buf) - (size_t)(end - s_tx_buf)),
                 ",\"auto\":%s,\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu}",
                 automation_is_enabled() ? "true" : "false",
                 net_mqtt_rssi(),
                 net_mqtt_ip_str(),
                 (unsigned long)(millis() / 1000UL));
    // snprintf 返回的是"本该写多少"，没算结尾那个零，所以留的位数要减一
    if (n <= 0 || n >= (int)(sizeof(s_tx_buf) - (size_t)(end - s_tx_buf) - 1)) {
        Serial.printf("BLE: 状态报文补字段后装不下（缓冲 %u），本条不上报\n",
                      (unsigned)sizeof(s_tx_buf));
        return;
    }

    ble_send_frame(BLE_UP_STATE, s_tx_buf, (int)strlen(s_tx_buf));
}

// 收到手机请求，三样一次给全
static void ble_send_all_now(void) {
    Serial.printf("BLE: 手机请求刷新，回 state + sensor + config\n");
    ble_send_state_now();
    ble_send_sensor_now();
    ble_send_config_now();
}

// 把 JSON 里一个个键的名字挑出来
// 手写个小扫描，省得为一个配置报文再拖一个 JSON 库进来
static int ble_json_keys(const char *json, char keys[][24], int max_keys) {
    const char *p = json;
    int n = 0;

    if (json == NULL || keys == NULL || max_keys <= 0) {
        return 0;
    }

    while ((p = strchr(p, '"')) != NULL) {
        const char *start = p + 1;
        const char *end = strchr(start, '"');
        size_t len;

        if (end == NULL) {
            break;
        }

        len = (size_t)(end - start);
        p = end + 1;

        if (len == 0 || len >= 24) {
            continue;
        }
        // 值也是带引号的，看后面紧跟的冒号才算键
        if (*p != ':') {
            continue;
        }

        memcpy(keys[n], start, len);
        keys[n][len] = '\0';
        n++;

        if (n >= max_keys) {
            break;
        }
    }

    return n;
}

// 收到阈值配置
// 一个个键丢给联动去改，改完存盘，再回一份完整的让手机确认
// 这个在主循环里跑，改联动配置不跟蓝牙任务抢
static void ble_handle_config_frame(const char *json, int len) {
    char keys[16][24];
    int  count;
    int  applied  = 0;
    int  rejected = 0;
    int  i;

    if (json == NULL || len <= 0) {
        Serial.printf("BLE: config 帧没有 JSON 内容，已忽略\n");
        return;
    }

    count = ble_json_keys(json, keys, (int)(sizeof(keys) / sizeof(keys[0])));
    if (count <= 0) {
        Serial.printf("BLE: config 帧里没认出任何键，已忽略\n");
        return;
    }

    for (i = 0; i < count; i++) {
        double value = 0.0;

        if (!json_get_num(json, keys[i], &value)) {
            // 真值和布尔以外的都跳过
            Serial.printf("BLE: config 跳过非数字键 \"%s\"\n", keys[i]);
            rejected++;
            continue;
        }

        if (automation_set_threshold(keys[i], (float)value)) {
            applied++;
            Serial.printf("BLE: config %s = %g\n", keys[i], value);
        } else {
            // 越界或者联动不认识这条线
            rejected++;
            Serial.printf("BLE: config \"%s\" = %g 被拒（越界或名字不认）\n", keys[i], value);
        }
    }

    automation_save();
    // 回一份最新的，手机拿这个刷新界面
    ble_send_config_now();

    Serial.printf("BLE: config 处理完 %d 条生效，%d 条被拒\n", applied, rejected);
}

// 收到手机发来的一整帧
// 只可能在主循环里被调，设备表和自动化随便碰
static void ble_handle_frame(uint8_t kind, const uint8_t *payload, uint16_t len) {
    const char *json;
    int json_len;

    Serial.printf("BLE: 处理帧 type=0x%02X len=%u\n", (unsigned)kind, (unsigned)len);

    if (payload == NULL || len == 0) {
        // GET 那种只有帧头没有内容
        if (kind == BLE_DOWN_GET) {
            ble_send_all_now();
            return;
        }
        Serial.printf("BLE: 帧没有内容，已忽略\n");
        return;
    }

    json = (const char *)payload;
    json_len = (int)len;

    switch (kind) {
    case BLE_DOWN_CMD:
        // 和网络那边共用一套解析，来源标成手机蓝牙
        app_cmd_handle_json(json, json_len, SRC_BLE);
        break;

    case BLE_DOWN_CONFIG:
        ble_handle_config_frame(json, json_len);
        break;

    case BLE_DOWN_GET:
        ble_send_all_now();
        break;

    default:
        Serial.printf("BLE: 未知帧类型 0x%02X，已忽略（len=%u）\n",
                      (unsigned)kind, (unsigned)len);
        break;
    }
}

// ----------------------------------------------------------------------
// 回调类
//
// 1.x 和 2.x 的回调签名不一样，两套都写一份，真正的活都在上面那些普通函数里，
// 哪套被调到都行。用 BLE_NIMBLE_V2 把新版那套包起来：
// 老版本里 NimBLEConnInfo 这个类型可能压根没有，写进去会直接编译报错。
//
// 只覆写确实存在的那几个口。多写一个基类里没有的同名函数不报错，
// 但它会悄悄盖掉基类那个虚函数，回调从此再也进不来，所以宁缺勿滥：
//   onSubscribe 就不覆写了（1.4 和 1.5 的参数都不一样），
//   订阅状态改成发之前直接问特征，见 BLE_TX_SUBSCRIBED。
// ----------------------------------------------------------------------

class BleServerCallbacks : public NimBLEServerCallbacks {
public:
    // 老版签名：就一个服务器指针
    void onConnect(NimBLEServer *server) {
        (void)server;
        ble_handle_connect(0xFFFF);
    }

    void onDisconnect(NimBLEServer *server) {
        (void)server;
        ble_handle_disconnect();
    }

#if BLE_NIMBLE_V2
    // 新版签名：多带一个连接信息，断开那个再带一个原因码
    void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) {
        (void)server;
        ble_handle_connect(connInfo.getConnHandle());
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) {
        (void)server;
        (void)connInfo;
        Serial.printf("BLE: 断开原因码 %d\n", reason);
        ble_handle_disconnect();
    }
#endif
};

class BleRxCallbacks : public NimBLECharacteristicCallbacks {
public:
    // 老版签名：手机写命令进来
    // 这儿是蓝牙任务，只把报文塞进队列，别的不碰，马上返回
    // 值先落到局部变量上，别让取值的临时对象先没了
    void onWrite(NimBLECharacteristic *chr) {
        NimBLEAttValue value = chr->getValue();
        // 1.x 的 c_str() 给的是 const char*，队列要 const uint8_t*，显式转一下
        // （2.x 的 NimBLEAttValue::c_str() 本来就是 const uint8_t*，转了也无害）
        ble_queue_rx((const uint8_t *)value.c_str(), (uint16_t)value.size());
    }

#if BLE_NIMBLE_V2
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) {
        (void)connInfo;
        NimBLEAttValue value = chr->getValue();
        // 1.x 的 c_str() 给的是 const char*，队列要 const uint8_t*，显式转一下
        // （2.x 的 NimBLEAttValue::c_str() 本来就是 const uint8_t*，转了也无害）
        ble_queue_rx((const uint8_t *)value.c_str(), (uint16_t)value.size());
    }
#endif

private:
    // 两版回调最后都落到这儿
    // 长度单算，别用 strlen，值里头可能带着结束零
    static void ble_queue_rx(const uint8_t *raw, uint16_t raw_len) {
        uint8_t kind;
        uint16_t json_len;

        if (raw == NULL || raw_len < 2) {
            // 空帧不算错，直接扔
            Serial.printf("BLE: 收到空帧，已忽略\n");
            return;
        }

        // 头一字节认帧
        kind = raw[0];
        if (kind != BLE_DOWN_CMD && kind != BLE_DOWN_CONFIG && kind != BLE_DOWN_GET) {
            Serial.printf("BLE: 未知帧类型 0x%02X，已忽略（len=%u）\n",
                          (unsigned)kind, (unsigned)raw_len);
            return;
        }

        json_len = (uint16_t)(raw_len - 1);
        if (kind == BLE_DOWN_GET) {
            // 只有帧头，别当成空帧给扔了
            json_len = 0;
        } else if (json_len == 0) {
            Serial.printf("BLE: 帧 type=0x%02X 没有 JSON 内容，已忽略\n", (unsigned)kind);
            return;
        }

        // 真正干活在 net_ble_poll 里，这儿只排队
        (void)ble_queue_push(kind, raw + 1, json_len);
    }
};

// TX 特征不该有人读，被读了就是用法不对
class BleTxCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic *chr) {
        (void)chr;
        Serial.printf("BLE: TX 特征被读，这边只发通知，已忽略\n");
    }

#if BLE_NIMBLE_V2
    void onRead(NimBLECharacteristic *chr, NimBLEConnInfo &connInfo) {
        (void)chr;
        (void)connInfo;
        Serial.printf("BLE: TX 特征被读，这边只发通知，已忽略\n");
    }
#endif
};

#endif  // APP_BLE_ENABLE

// ======================================================================
// 对外那几个口
// ======================================================================

#if APP_BLE_ENABLE

bool net_ble_init(void) {
    NimBLEServer *server = NULL;
    NimBLEService *svc = NULL;

    if (s_inited) {
        Serial.printf("BLE: 已经启动过，跳过\n");
        return true;
    }

    // 先把广播名拼好，后面 init 和广播都要用
    ble_build_device_name();

    BLE_INIT_DEVICE(s_dev_name);

    server = NimBLEDevice::createServer();
    if (server == NULL) {
        Serial.printf("BLE: createServer 失败，手机蓝牙控制不可用\n");
        return false;
    }
    server->setCallbacks(new BleServerCallbacks());

    // 建服务的口两个版本都一样，就吃一个 UUID，别多给参数
    // 新版多出来的那两个参数（num handles / inst id）老版不认，写上去就编不过
    svc = server->createService(BLE_SVC_UUID);
    if (svc == NULL) {
        // 服务都建不出来就别装作能用，把协议栈放掉
        Serial.printf("BLE: 建服务 %s 失败，手机蓝牙控制不可用\n", BLE_SVC_UUID);
        NimBLEDevice::deinit(true);
        return false;
    }

    // 手机写命令进来，两种写都放开
    // 第三个参数是特征值的最大长度，不给就看库里的默认值
    s_rx_chr = svc->createCharacteristic(
        BLE_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    if (s_rx_chr != NULL) {
        s_rx_chr->setCallbacks(new BleRxCallbacks());
    }

    // 板子推给手机，只开通知
    s_tx_chr = svc->createCharacteristic(BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    if (s_tx_chr != NULL) {
        s_tx_chr->setCallbacks(new BleTxCallbacks());
    }

    if (s_rx_chr == NULL || s_tx_chr == NULL) {
        Serial.printf("BLE: 建特征失败（RX=%d TX=%d），手机蓝牙控制不可用\n",
                      (int)(s_rx_chr != NULL), (int)(s_tx_chr != NULL));
        NimBLEDevice::deinit(true);
        s_rx_chr = NULL;
        s_tx_chr = NULL;
        return false;
    }

    // 老版靠 server->start() 把 GATT 挂上；新版说服务跟着服务器一起起，
    // 这个口还在，只是空转，调了不亏，两边都编得过
    server->start();
    ble_start_advertising();

    s_inited = true;
    Serial.printf("BLE: 蓝牙已就绪，广播名 \"%s\"，服务 %s\n", s_dev_name, BLE_SVC_UUID);
    return true;
}

void net_ble_poll(void) {
    uint8_t  kind  = 0;
    uint16_t len   = 0;
    uint8_t  payload[BLE_QUEUE_PAYLOAD];
    // 一轮最多处理这么多条，手机抽风连发也不会把主循环卡住
    int      budget = BLE_QUEUE_SLOTS;
    bool     got;

    // 初始化都没成，别假装还在跑
    if (!s_inited) {
        return;
    }

    // 关掉蓝牙再打开、或者广播自己停了，这里补一次
    // 不断言、不延时，就一次调用
    if (!s_connected && !s_advertising) {
        ble_start_advertising();
    }

    // 把蓝牙任务排进来的命令在主循环里做掉，两边不同时碰设备表
    do {
        got = ble_queue_pop(&kind, payload, &len);
        if (got) {
            ble_handle_frame(kind, payload, len);
            budget--;
        }
    } while (got && budget > 0);
}

bool net_ble_is_connected(void) {
    return s_connected;
}

void net_ble_notify_state(void) {
    ble_send_state_now();
}

void net_ble_notify_sensor(void) {
    ble_send_sensor_now();
}

void net_ble_notify_config(void) {
    ble_send_config_now();
}

#else  // APP_BLE_ENABLE == 0

// 没装 NimBLE 库、或者 app_config.h 里把开关关了：全空着，保证还能编过
// 这一支里没有 include NimBLEDevice.h，所以库没装也照样编

bool net_ble_init(void) {
    Serial.printf("BLE: 未编译进固件（APP_BLE_ENABLE=0），手机蓝牙控制不可用；其余功能不受影响\n");
    return false;
}

void net_ble_poll(void) {
    // 没开蓝牙，什么都不用维护
}

bool net_ble_is_connected(void) {
    return false;
}

void net_ble_notify_state(void) {
    // 没链路，直接扔
}

void net_ble_notify_sensor(void) {
    // 没链路，直接扔
}

void net_ble_notify_config(void) {
    // 没链路，直接扔
}

#endif  // APP_BLE_ENABLE
