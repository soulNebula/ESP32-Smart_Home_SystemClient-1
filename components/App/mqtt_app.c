/**
 * @file  mqtt_app.c
 * @brief MQTT 客户端实现（esp-mqtt）—— 对应 mqtt_app.h
 *
 * 一句话数据流：
 *   手机 App --(base/cmd、base/config、base/get)--> 本文件解析 --> device_model / automation
 *   device_model 状态变化 --(回调)--> base/state
 *   传感器定时任务 --> mqtt_publish_sensor() --> base/sensor
 *
 * 线程安全说明：
 *   esp_mqtt_client_publish() / esp_mqtt_client_subscribe() 内部有锁，是线程安全的，
 *   可以在 MQTT 事件回调里、按键/语音任务里、定时器里直接调用。
 *   本文件所有 publish 函数在未连接时静默返回 ESP_ERR_INVALID_STATE（不打错误日志），
 *   避免断网时刷屏。
 */

#include "mqtt_app.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "app_link.h"
#include "app_cmd.h"

#include "mqtt_protocol.h"
#include "wifi_sta.h"
#include "device_model.h"
#include "automation.h"
#include "sensor.h"
#include "led.h"

static const char *TAG = "mqtt_app";

/* topic 缓冲：base（前缀 + '/' + 6 位 ID）约 20~40 字符，加后缀绰绰有余 */
#define TOPIC_BUF_LEN    128
/* state 快照缓冲：device_snapshot_json() 的输出 */
#define SNAP_BUF_LEN     1024
/* 下行 JSON 缓冲：topic / data 都不带 '\0'，必须自己按长度拷进来 */
#define RX_BUF_LEN       256
/* client_id 缓冲："esp32sh-" + uid */
#define CLIENT_ID_LEN    48

#define DEV_NAME_MAX     32
#define ACTION_MAX       24

/* ---------------- 模块状态 ---------------- */
static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;   /* CONNECTED/DISCONNECTED 里维护 */
static bool                     s_started   = false;   /* 幂等标志 */

static char s_client_id[CLIENT_ID_LEN] = { 0 };

/* topic 只在 mqtt_app_start() 里拼一次，之后只读 */
static char s_topic_state[TOPIC_BUF_LEN]        = { 0 };
static char s_topic_sensor[TOPIC_BUF_LEN]       = { 0 };
static char s_topic_availability[TOPIC_BUF_LEN] = { 0 };
static char s_topic_ack[TOPIC_BUF_LEN]          = { 0 };
static char s_topic_event[TOPIC_BUF_LEN]        = { 0 };
static char s_topic_cmd[TOPIC_BUF_LEN]          = { 0 };
static char s_topic_cmd_wild[TOPIC_BUF_LEN]     = { 0 };
static char s_topic_config[TOPIC_BUF_LEN]       = { 0 };

/* ---- 回环保护 ----
 * <base>/config 既是【下行】命令 topic（我们订阅它收配置），
 * 又是【上行】上报 topic（mqtt_publish_config() 往同一个 topic 发当前配置，retain=1）。
 * MQTT 3.1.1 没有 NoLocal 语义，有些 broker 会把消息回显给发布者。一旦回显：
 *   handle_config() 收到自己刚发的配置
 *     → automation_set_threshold() + automation_save() + mqtt_publish_config()
 *     → 又发一条 → 无限回环。
 * 这里记住"最后一次由我们自己发布"的 JSON，内容完全一致就判定为回显并忽略。
 * （只在对方发过一次 config 之后才会进入这条链路，所以基础联调不易发现。） */
static char s_last_pub_config[512] = { 0 };
static char s_topic_get[TOPIC_BUF_LEN]          = { 0 };

/* ---------------- 内部小工具 ---------------- */

/**
 * @brief 把"不带 '\0' 结尾"的 MQTT topic/data 安全拷进本地缓冲
 * @return true = 拷贝成功且加了 '\0'；false = 空/超长
 */
static bool copy_bounded(char *dst, size_t dst_size, const char *src, int src_len)
{
    if (dst == NULL || dst_size == 0 || src == NULL || src_len <= 0) {
        return false;
    }
    if ((size_t)src_len >= dst_size) {
        return false;
    }
    memcpy(dst, src, (size_t)src_len);
    dst[src_len] = '\0';
    return true;
}

/**
 * @brief topic 是否以 suffix 结尾（如 "/config"、"/get"）
 */
static bool topic_endswith(const char *topic, int topic_len, const char *suffix)
{
    size_t sl;

    if (topic == NULL || suffix == NULL || topic_len <= 0) {
        return false;
    }
    sl = strlen(suffix);
    if ((size_t)topic_len < sl) {
        return false;
    }
    return memcmp(topic + topic_len - sl, suffix, sl) == 0;
}

/**
 * @brief topic 是否命中 "<base>/cmd" 或它下面的子主题 "<base>/cmd/xxx"
 * @note  订阅用的是通配 "<base>/cmd/#"，所以两种情况都要认
 */
static bool topic_is_cmd(const char *topic, int topic_len)
{
    size_t cl;

    if (topic == NULL || topic_len <= 0) {
        return false;
    }
    cl = strlen(s_topic_cmd);
    if ((size_t)topic_len < cl) {
        return false;
    }
    if (memcmp(topic, s_topic_cmd, cl) != 0) {
        return false;
    }
    return ((size_t)topic_len == cl) || (topic[cl] == '/');
}

/* ---------------- 下行：命令 JSON ---------------- */

void app_cmd_handle_json(const char *payload, int payload_len, ctrl_source_t src)
{
    char json[RX_BUF_LEN];
    char dev[DEV_NAME_MAX] = { 0 };
    char action[ACTION_MAX] = { 0 };
    const cJSON *jdev    = NULL;
    const cJSON *jaction = NULL;
    const cJSON *jvalue  = NULL;
    cJSON *root;
    esp_err_t err = ESP_OK;
    int value = 100;                /* {"action":"set"} 缺省亮度/转速 */
    int r = 255, g = 255, b = 255;  /* {"action":"color"} 缺省白色 */
    device_id_t id;

    if (!copy_bounded(json, sizeof(json), payload, payload_len)) {
        ESP_LOGE(TAG, "cmd: empty or >%u bytes payload", (unsigned)sizeof(json));
        mqtt_publish_ack("cmd", false, "payload too long");
        return;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "cmd: bad json \"%s\"", json);
        mqtt_publish_ack("cmd", false, "bad json");
        return;
    }

    jdev    = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_DEV);
    jaction = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_ACTION);
    jvalue  = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_VALUE);

    if (jaction == NULL || !cJSON_IsString(jaction) || jaction->valuestring == NULL ||
        strlen(jaction->valuestring) >= sizeof(action)) {
        ESP_LOGE(TAG, "cmd: missing or illegal \"%s\"", MQTT_KEY_ACTION);
        cJSON_Delete(root);
        mqtt_publish_ack("cmd", false, "missing action");
        return;
    }

    /* ★ 先把字符串拷出来 —— cJSON_Delete(root) 之后 valuestring 就失效了，
     *   后面发 ack 时还要用，绝不能留悬空指针。 */
    snprintf(action, sizeof(action), "%s", jaction->valuestring);

    if (jdev != NULL && cJSON_IsString(jdev) && jdev->valuestring != NULL) {
        if (strlen(jdev->valuestring) >= sizeof(dev)) {
            ESP_LOGE(TAG, "cmd: \"%s\" too long", MQTT_KEY_DEV);
            cJSON_Delete(root);
            mqtt_publish_ack(action, false, "dev too long");
            return;
        }
        snprintf(dev, sizeof(dev), "%s", jdev->valuestring);
    }

    /* ---- {"action":"auto","value":true} ：不带 dev，切自动模式总开关 ---- */
    if (strcmp(action, MQTT_ACTION_AUTO) == 0) {
        bool on = true;

        if (jvalue != NULL) {
            if (cJSON_IsBool(jvalue)) {
                on = cJSON_IsTrue(jvalue) ? true : false;
            } else if (cJSON_IsNumber(jvalue)) {
                on = (jvalue->valuedouble != 0.0);
            }
        }
        cJSON_Delete(root);

        automation_set_enabled(on);
        automation_save();
        ESP_LOGI(TAG, "auto mode -> %s", on ? "ON" : "OFF");
        mqtt_publish_ack(action, true, on ? "auto on" : "auto off");
        mqtt_publish_config();
        return;
    }

    if (dev[0] == '\0') {
        cJSON_Delete(root);
        mqtt_publish_ack(action, false, "missing dev");
        return;
    }

    /* ---- dev == "all" ----
     * device_from_name("all") 会返回 DEV_COUNT（和其它"不认识"的名字一样），
     * 所以必须先单独判字符串。 */
    if (strcmp(dev, "all") == 0) {
        if (strcmp(action, MQTT_ACTION_OFF) == 0) {
            err = device_all_off(src);
        } else if (strcmp(action, MQTT_ACTION_ON) == 0) {
            /* ★ "全开" 只开 4 路灯带 + 风扇（DEV_LED_LIVING..DEV_FAN）。
             *   故意【不】去开窗/门/窗帘 —— 把门"全开"是安全隐患。
             *   要开合门窗请用 {"dev":"door","action":"open"} 这种单设备命令。
             *   main.c 串口调试台的 "on all" 与此保持一致。 */
            for (int i = (int)DEV_LED_LIVING; i <= (int)DEV_FAN; i++) {
                esp_err_t e = device_set_power((device_id_t)i, true, src);
                if (e != ESP_OK && err == ESP_OK) {
                    err = e;
                }
            }
        } else {
            ESP_LOGW(TAG, "cmd: dev=all only supports on/off (got \"%s\")", action);
            cJSON_Delete(root);
            mqtt_publish_ack(action, false, "all: only on/off");
            return;
        }
        cJSON_Delete(root);

        mqtt_publish_ack(action, err == ESP_OK, err == ESP_OK ? "all" : esp_err_to_name(err));
        if (err == ESP_OK) {
            mqtt_publish_state();
        }
        return;
    }

    id = device_from_name(dev);
    if (id >= DEV_COUNT) {
        ESP_LOGW(TAG, "cmd: unknown dev \"%s\"", dev);
        cJSON_Delete(root);
        mqtt_publish_ack(action, false, "unknown dev");
        return;
    }

    /* ---- 取数值参数（cJSON 支持 number / bool 两种写法） ---- */
    if (jvalue != NULL) {
        if (cJSON_IsNumber(jvalue)) {
            value = (int)jvalue->valuedouble;
        } else if (cJSON_IsBool(jvalue)) {
            value = cJSON_IsTrue(jvalue) ? 100 : 0;
        } else {
            ESP_LOGW(TAG, "cmd: \"%s\" is not a number, using %d", MQTT_KEY_VALUE, value);
        }
    }
    {
        const cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_R);
        const cJSON *jg = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_G);
        const cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, MQTT_KEY_B);

        if (jr != NULL && cJSON_IsNumber(jr)) {
            r = (int)jr->valuedouble;
        }
        if (jg != NULL && cJSON_IsNumber(jg)) {
            g = (int)jg->valuedouble;
        }
        if (jb != NULL && cJSON_IsNumber(jb)) {
            b = (int)jb->valuedouble;
        }
    }
    cJSON_Delete(root);

    /* 值域收敛，免得 App 端传来离谱的数 */
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    if (r < 0)   { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0)   { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0)   { b = 0; }
    if (b > 255) { b = 255; }

    /* ---- 执行 ---- */
    if (strcmp(action, MQTT_ACTION_ON) == 0) {
        err = device_set_power(id, true, src);
    } else if (strcmp(action, MQTT_ACTION_OFF) == 0) {
        err = device_set_power(id, false, src);
    } else if (strcmp(action, MQTT_ACTION_TOGGLE) == 0) {
        err = device_toggle(id, src);
    } else if (strcmp(action, MQTT_ACTION_SET) == 0) {
        err = device_set_level(id, (uint8_t)value, src);
    } else if (strcmp(action, MQTT_ACTION_OPEN) == 0) {
        err = device_set_level(id, 100, src);      /* open  ≡ set value=100 */
    } else if (strcmp(action, MQTT_ACTION_CLOSE) == 0) {
        err = device_set_level(id, 0, src);        /* close ≡ set value=0   */
    } else if (strcmp(action, MQTT_ACTION_COLOR) == 0) {
        err = device_set_color(id, (uint8_t)r, (uint8_t)g, (uint8_t)b, src);
    } else {
        ESP_LOGW(TAG, "cmd: unsupported action \"%s\"", action);
        mqtt_publish_ack(action, false, "unsupported action");
        return;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cmd ok: dev=%s action=%s value=%d rgb=%d,%d,%d",
                 dev, action, value, r, g, b);
        mqtt_publish_ack(action, true, dev);
        /* device_model 的状态回调（mqtt_app_bind_device_events 注册的）通常已经发过 state；
         * 这里再发一次是兜底：万一 main.c 没调用 bind，手机端也不会看到过时状态。 */
        mqtt_publish_state();
    } else {
        ESP_LOGE(TAG, "cmd failed: dev=%s action=%s (%s)", dev, action, esp_err_to_name(err));
        mqtt_publish_ack(action, false, esp_err_to_name(err));
    }
}

/* ---------------- 下行：阈值配置 JSON ---------------- */

void app_cmd_handle_config_json(const char *payload, int payload_len, ctrl_source_t src)
{
    char json[RX_BUF_LEN];
    cJSON *root;
    const cJSON *item = NULL;
    int applied  = 0;
    int rejected = 0;

    if (!copy_bounded(json, sizeof(json), payload, payload_len)) {
        ESP_LOGE(TAG, "config: empty or >%u bytes payload", (unsigned)sizeof(json));
        return;
    }

    /* 回环保护：如果这条正是我们自己刚发布的配置（broker 回显），直接忽略 */
    /* 回环保护：如果这条正是我们自己刚发布的配置（broker 回显），直接忽略。
     * ⚠ 只对 MQTT 生效 —— BLE 没有 broker 回显这回事，而且手机刚下发的配置
     *   内容恰好与上一次 MQTT 发布的相同时，不该被误判成回显而丢掉。 */
    if (src == SRC_MQTT && s_last_pub_config[0] != '\0' &&
        strcmp(json, s_last_pub_config) == 0) {
        ESP_LOGD(TAG, "config: ignore echo of our own published config");
        return;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "config: bad json \"%s\"", json);
        mqtt_publish_ack("config", false, "bad json");
        return;
    }

    /* 遍历每个子项：键名原样交给 automation_set_threshold()，
     * 布尔项（enabled / auto_*_enable / auto_window_reopen）转成 1.0 / 0.0。 */
    cJSON_ArrayForEach(item, root) {
        esp_err_t err;
        float v;

        if (item->string == NULL) {
            continue;
        }
        if (cJSON_IsBool(item)) {
            v = cJSON_IsTrue(item) ? 1.0f : 0.0f;
        } else if (cJSON_IsNumber(item)) {
            v = (float)item->valuedouble;
        } else {
            ESP_LOGW(TAG, "config: skip non-numeric key \"%s\"", item->string);
            rejected++;
            continue;
        }

        err = automation_set_threshold(item->string, v);
        if (err == ESP_OK) {
            applied++;
            ESP_LOGI(TAG, "config: %s = %g", item->string, (double)v);
        } else {
            rejected++;
            ESP_LOGW(TAG, "config: \"%s\" = %g rejected (%s)", item->string, (double)v,
                     esp_err_to_name(err));
        }
    }
    cJSON_Delete(root);

    automation_save();
    mqtt_publish_config();
    mqtt_publish_ack("config", rejected == 0, (rejected == 0) ? "applied" : "partly applied");
    ESP_LOGI(TAG, "config done: %d applied, %d rejected", applied, rejected);
}

/* ---------------- 收到消息后按 topic 分发 ---------------- */

static void handle_message(const char *topic, int topic_len, const char *data, int data_len)
{
    if (topic == NULL || topic_len <= 0) {
        ESP_LOGW(TAG, "message without topic, ignored");
        return;
    }

    if (topic_is_cmd(topic, topic_len)) {
        app_cmd_handle_json(data, data_len, SRC_MQTT);
        return;
    }
    if (topic_endswith(topic, topic_len, "/config")) {
        app_cmd_handle_config_json(data, data_len, SRC_MQTT);
        return;
    }
    if (topic_endswith(topic, topic_len, "/get")) {
        ESP_LOGI(TAG, "get request: reply state + sensor");
        mqtt_publish_state();
        mqtt_publish_sensor(NULL);
        return;
    }

    ESP_LOGW(TAG, "unhandled topic: %.*s", topic_len, topic);
}

/* ---------------- 上行：publish ---------------- */

esp_err_t mqtt_publish_state(void)
{
    /* ★ 这个缓冲以前是栈上 char snap[1024]：key_scan / voice_rx 任务的栈只有
     * 3072B，回调链里再叠 cJSON 递归会直接把栈压爆（实测 KEY1 单击 100% panic，
     * LoadProhibited / InstrFetchProhibited）。改成堆分配后所有调用任务都受益。 */
    char *snap = (char *)malloc(SNAP_BUF_LEN);
    char ip[16] = "0.0.0.0";
    cJSON *root;
    char *json;
    int n;

    if (snap == NULL) {
        ESP_LOGE(TAG, "malloc(%u) for state snapshot failed", (unsigned)SNAP_BUF_LEN);
        return ESP_ERR_NO_MEM;
    }

    n = device_snapshot_json(snap, SNAP_BUF_LEN);
    if (n <= 0 || n >= (int)SNAP_BUF_LEN) {
        /* n<=0 说明缓冲不够；n>=sizeof 说明连结尾的 '\0' 都没地方放。
         * 两种都必须安全退出，绝不能拿半个 JSON 去 cJSON_Parse。 */
        ESP_LOGE(TAG, "device_snapshot_json failed (ret=%d, buf=%u)", n, (unsigned)SNAP_BUF_LEN);
        free(snap);
        return ESP_ERR_INVALID_SIZE;
    }
    snap[n] = '\0';   /* 双保险：万一实现只返回长度而没落 '\0' */

    /* device_snapshot_json() 只输出设备状态，state 还要带 auto/rssi/ip/uptime */
    root = cJSON_Parse(snap);
    if (root == NULL) {
        ESP_LOGE(TAG, "state snapshot is not valid json");
        free(snap);
        return ESP_ERR_INVALID_ARG;
    }
    free(snap);   /* cJSON_Parse 已把内容拷进自己的树，源缓冲可以释放 */

    (void)wifi_get_ip_str(ip, sizeof(ip));
    cJSON_AddBoolToObject(root, "auto", automation_is_enabled());
    cJSON_AddNumberToObject(root, "rssi", (double)wifi_get_rssi());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);           /* ★ Parse 出来的必须 Delete */
    if (json == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (no mem)");
        return ESP_ERR_NO_MEM;
    }

    /* 广播给所有已连接链路：MQTT 发到 <base>/state，BLE 发 notify。
     * 未连接的链路会被 app_link_broadcast() 自动跳过。 */
    app_link_broadcast(APP_MSG_STATE, json, strlen(json));
    ESP_LOGD(TAG, "state published: %s", json);
    cJSON_free(json);             /* ★ Print 出来的必须 free；上面日志要用，所以放到最后 */

    return ESP_OK;
}

esp_err_t mqtt_publish_sensor(const sensor_data_t *d)
{
    cJSON *root;
    char *json;

    if (d == NULL) {
        d = sensor_get_last();     /* 约定：永不为 NULL，但还是防一手 */
    }
    if (d == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 字段名严格按 mqtt_protocol.h 的文档 */
    cJSON_AddNumberToObject(root, "temp", (double)d->temperature);
    cJSON_AddNumberToObject(root, "humi", (double)d->humidity);
    cJSON_AddBoolToObject(root, "temp_valid", d->valid_temp);
    cJSON_AddNumberToObject(root, "lux", (double)d->lux);
    cJSON_AddNumberToObject(root, "light_pct", (double)d->light_pct);
    cJSON_AddBoolToObject(root, "light_is_bh1750", d->light_is_bh1750);
    cJSON_AddNumberToObject(root, "rain", (double)d->rain_pct);
    cJSON_AddBoolToObject(root, "rain_detected", d->rain_detected);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 广播给所有链路。MQTT 侧仍是 QoS0 + 不 retain（1~2s 一条，量大）；
     * BLE 侧就是一条 notify，本身不重传。 */
    app_link_broadcast(APP_MSG_SENSOR, json, strlen(json));
    cJSON_free(json);

    return ESP_OK;
}

esp_err_t mqtt_publish_config(void)
{
    char buf[512];
    int n;

    n = automation_cfg_json(buf, sizeof(buf));
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        ESP_LOGE(TAG, "automation_cfg_json failed (ret=%d, buf=%u)", n, (unsigned)sizeof(buf));
        return ESP_ERR_INVALID_SIZE;
    }
    buf[n] = '\0';   /* 双保险：下面要按字符串打日志 */

    /* 记下这次发出去的内容，供 handle_config() 识别 broker 回显 */
    strncpy(s_last_pub_config, buf, sizeof(s_last_pub_config) - 1);
    s_last_pub_config[sizeof(s_last_pub_config) - 1] = '\0';

    app_link_broadcast(APP_MSG_CONFIG, buf, (size_t)n);
    ESP_LOGI(TAG, "config published: %s", buf);
    return ESP_OK;
}

esp_err_t mqtt_publish_ack(const char *what, bool ok, const char *detail)
{
    cJSON *root;
    char *json;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "action", (what != NULL) ? what : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "detail", (detail != NULL) ? detail : "");

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    app_link_broadcast(APP_MSG_ACK, json, strlen(json));
    cJSON_free(json);

    return ESP_OK;
}

esp_err_t mqtt_publish_event(const char *what)
{
    cJSON *root;
    char *json;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "event", (what != NULL) ? what : "");

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 按键/语音事件是"人操作了一次"，丢了会让 App 漏掉一次动作。
     * MQTT 侧用 QoS1 不 retain；BLE 侧就是一条 notify。 */
    app_link_broadcast(APP_MSG_EVENT, json, strlen(json));
    cJSON_free(json);
    return ESP_OK;
}

/* ---------------- MQTT 作为 app_link 的一条链路 ---------------- */

static bool mqtt_link_is_connected(void)
{
    return s_connected;
}

/**
 * @brief 把广播来的一条 JSON 发到对应的 MQTT topic
 * @note  topic / QoS / retain 的对应关系与原实现【逐字保持一致】：
 *          state  → qos1 retain1  手机重连后立刻能看到当前状态
 *          sensor → qos0 retain0  1~2s 一条量大，丢了无所谓
 *          ack    → qos1 retain0  命令回执不能丢
 *          event  → qos1 retain0  按键/语音事件不能丢
 *          config → qos1 retain1  进 App 就能拿到当前阈值
 */
static esp_err_t mqtt_link_send(app_msg_type_t type, const char *json, size_t len)
{
    const char *topic  = NULL;
    int         qos    = 0;
    int         retain = 0;

    if (!s_connected || s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (type) {
    case APP_MSG_STATE:  topic = s_topic_state;  qos = 1; retain = 1; break;
    case APP_MSG_SENSOR: topic = s_topic_sensor; qos = 0; retain = 0; break;
    case APP_MSG_ACK:    topic = s_topic_ack;    qos = 1; retain = 0; break;
    case APP_MSG_EVENT:  topic = s_topic_event;  qos = 1; retain = 0; break;
    case APP_MSG_CONFIG: topic = s_topic_config; qos = 1; retain = 1; break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (topic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;   /* topic 还没拼好（init 之前） */
    }

    const int msg_id = esp_mqtt_client_publish(s_client, topic, json, (int)len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish %s failed (msg_id=%d)", app_msg_type_name(type), msg_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const app_link_t s_mqtt_link = {
    .name         = "mqtt",
    .send         = mqtt_link_send,
    .is_connected = mqtt_link_is_connected,
};

bool mqtt_is_connected(void)
{
    return s_connected;
}

/* ---------------- 设备状态变化 → 自动上报 ---------------- */

static void device_changed_cb(device_id_t id, ctrl_source_t src, void *user_data)
{
    (void)user_data;

    /* ⚠ 本回调可能在【MQTT 任务】、【按键任务】、【语音任务】或【自动联动任务】
     *   的上下文里执行。esp_mqtt_client_publish() 是线程安全的（内部有互斥锁），
     *   所以这里直接发是安全的；未连接时 mqtt_publish_state() 会静默返回。 */
    ESP_LOGD(TAG, "device changed: %s (by %s) -> publish state",
             device_id_name(id), ctrl_source_name(src));
    (void)mqtt_publish_state();
}

esp_err_t mqtt_app_bind_device_events(void)
{
    /* ★ 注意：device_register_cb()【只保留最后一个】回调 ——
     *   本函数【独占】device_model 的回调槽位。若还需要 OLED 刷新等其它订阅者，
     *   必须先让 device_model 支持多订阅（或在 main.c 里做一次回调分发），
     *   否则后注册的会把先注册的顶掉。 */
    return device_register_cb(device_changed_cb, NULL);
}

/* ---------------- MQTT 事件回调 ---------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = (event != NULL) ? event->client : s_client;

    (void)handler_args;
    (void)base;

    if (event == NULL) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to %s (client_id=%s)", CONFIG_APP_MQTT_BROKER_URI, s_client_id);
        s_connected = true;
        led_status_set(LED_STATUS_MQTT_OK);

        /* 订阅下行：命令通配 + 配置 + 主动查询 */
        esp_mqtt_client_subscribe(client, s_topic_cmd_wild, 1);
        esp_mqtt_client_subscribe(client, s_topic_config, 1);
        esp_mqtt_client_subscribe(client, s_topic_get, 1);

        /* 上线通告（retain，让后连上的 App 也能立刻看到 online） */
        esp_mqtt_client_publish(client, s_topic_availability, MQTT_PAYLOAD_ONLINE, 0, 1, 1);

        /* 立刻主动上报一次，App 一连上就有数据 */
        mqtt_publish_state();
        mqtt_publish_sensor(sensor_get_last());
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker, esp-mqtt will reconnect automatically");
        s_connected = false;
        led_status_set(LED_STATUS_ERROR);
        break;

    case MQTT_EVENT_DATA:
        /* ⚠ topic 和 data 都【不是】以 '\0' 结尾的，必须按长度处理；
         *   handle_message() 内部会拷进局部缓冲再加 '\0'。 */
        if (event->current_data_offset != 0) {
            /* 一条消息超过内部缓冲会被拆成多次事件，这里只处理第一片。
             * 本项目的下行 JSON 都很短（<256B），分片属于异常情况。 */
            ESP_LOGW(TAG, "ignore fragmented payload (offset=%d, total=%d)",
                     event->current_data_offset, event->total_data_len);
            break;
        }
        ESP_LOGD(TAG, "data: topic=%.*s", event->topic_len, event->topic);
        handle_message(event->topic, event->topic_len, event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR, error_type=%d", event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  tls_last_esp_err=0x%x, tls_stack_err=0x%x, sock_errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->esp_transport_sock_errno);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "  connection refused, return code=0x%x",
                     event->error_handle->connect_return_code);
        }
        break;

    default:
        ESP_LOGD(TAG, "other event id=%d", (int)event_id);
        break;
    }
}

/* ---------------- 启动 ---------------- */

/** @brief 拼 topic，失败就打日志返回 */
#define BUILD_TOPIC(dst, func)                                                    \
    do {                                                                          \
        esp_err_t e_ = func((dst), sizeof(dst));                                   \
        if (e_ != ESP_OK) {                                                        \
            ESP_LOGE(TAG, "build topic " #func " failed: %s", esp_err_to_name(e_)); \
            return e_;                                                             \
        }                                                                          \
    } while (0)

esp_err_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t cfg = { 0 };
    char uid[32] = { 0 };
    esp_err_t err;

    if (s_started) {
        ESP_LOGI(TAG, "already started, skip");
        return ESP_OK;
    }

    /* 1) 拼 topic（唯一 ID 在 mqtt_protocol.c 里统一处理） */
    BUILD_TOPIC(s_topic_state,        mqtt_topic_state);
    BUILD_TOPIC(s_topic_sensor,       mqtt_topic_sensor);
    BUILD_TOPIC(s_topic_availability, mqtt_topic_availability);
    BUILD_TOPIC(s_topic_ack,          mqtt_topic_ack);
    BUILD_TOPIC(s_topic_event,        mqtt_topic_event);
    BUILD_TOPIC(s_topic_cmd,          mqtt_topic_cmd);
    BUILD_TOPIC(s_topic_cmd_wild,     mqtt_topic_cmd_wildcard);
    BUILD_TOPIC(s_topic_config,       mqtt_topic_config);
    BUILD_TOPIC(s_topic_get,          mqtt_topic_get);

    /* 2) client_id 必须全局唯一，否则公共 broker 上两台设备会互相顶掉 */
    if (CONFIG_APP_MQTT_UID_OVERRIDE[0] != '\0') {
        snprintf(uid, sizeof(uid), "%s", CONFIG_APP_MQTT_UID_OVERRIDE);
    } else if (wifi_get_mac_suffix(uid, sizeof(uid)) != ESP_OK) {
        snprintf(uid, sizeof(uid), "%06x", (unsigned)(esp_random() & 0xFFFFFFu));
        ESP_LOGW(TAG, "mac unavailable, fallback uid=%s", uid);
    }
    snprintf(s_client_id, sizeof(s_client_id), "esp32sh-%s", uid);

    /* 3) 客户端配置
     *    IDF 5.x 的字段是嵌套结构，路径已对照 mqtt_client.h 逐个核对。 */
    cfg.broker.address.uri            = CONFIG_APP_MQTT_BROKER_URI;
    cfg.credentials.client_id         = s_client_id;
    cfg.session.keepalive             = CONFIG_APP_MQTT_KEEPALIVE_S;
    cfg.session.last_will.topic       = s_topic_availability;
    cfg.session.last_will.msg         = MQTT_PAYLOAD_OFFLINE;
    cfg.session.last_will.msg_len     = (int)strlen(MQTT_PAYLOAD_OFFLINE);
    cfg.session.last_will.qos         = 1;
    cfg.session.last_will.retain      = 1;      /* 该字段在 IDF 里是 int，不是 bool */
    cfg.network.reconnect_timeout_ms  = 5000;   /* 断线 5s 后重连 */
    cfg.buffer.size                   = 2048;   /* state 快照 JSON 需要空间 */

    /* 公共 broker 不需要认证：用户名/密码为空就【不设置】这两个字段 */
    if (CONFIG_APP_MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = CONFIG_APP_MQTT_USERNAME;
    }
    if (CONFIG_APP_MQTT_PASSWORD[0] != '\0') {
        cfg.credentials.authentication.password = CONFIG_APP_MQTT_PASSWORD;
    }

    /* 4) init / 注册事件 / start */
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    /* 5) 注册为 app_link 的一条链路。
     *    ⚠ 这一步是【必需的】：改造后 mqtt_publish_xxx() 不再自己发 MQTT，而是
     *      "拼 JSON + app_link_broadcast()"，MQTT 必须靠这条注册才收得到上行。
     *      注册失败等于没有上行通道，所以按致命错误处理。 */
    err = app_link_register(&s_mqtt_link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_link_register(mqtt) failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "started: broker=%s client_id=%s", CONFIG_APP_MQTT_BROKER_URI, s_client_id);
    ESP_LOGI(TAG, "topics: state=%s  cmd=%s/#  config=%s  get=%s",
             s_topic_state, s_topic_cmd, s_topic_config, s_topic_get);
    return ESP_OK;
}
