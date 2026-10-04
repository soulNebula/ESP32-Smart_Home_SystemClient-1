/*
 * 模块：
 *   连路由上云收命令这条链路。给 main.c 调用，收到命令就去改设备。
 *   手机走上网和走蓝牙下发的是同一套 JSON，认命令只有一份：
 *   声明在 app_cmd.h，实现在本文件，ble_app.c 也调这两个函数。
 *   往上收：命令、阈值、查询。往下调：device_model 改设备、automation 改联动、
 *   sensor 取测量、wifi_sta 看网络。回执和状态统一交给 app_link 广播，
 *   上网和蓝牙两条链路各拿一份，谁也不漏。
 *
 * 功能：
 *   连上服务器收命令
 *   认回显，防死循环
 *   解析命令
 *   收阈值改联动
 *   报状态和测量
 *   回执行结果
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

/* 功能：放主题名 */
#define TOPIC_BUF_LEN    128
/* 功能：放状态快照 */
#define SNAP_BUF_LEN     1024
/* 功能：放收到的命令 */
#define RX_BUF_LEN       256
/* 功能：放客户端名 */
#define CLIENT_ID_LEN    48

#define DEV_NAME_MAX     32
#define ACTION_MAX       24

static esp_mqtt_client_handle_t s_client    = NULL;
static volatile bool            s_connected = false;   /* 功能：连着才置真 */
static bool                     s_started   = false;   /* 功能：防重复启动 */

static char s_client_id[CLIENT_ID_LEN] = { 0 };

/* 功能：开机拼一次主题 */
static char s_topic_state[TOPIC_BUF_LEN]        = { 0 };
static char s_topic_sensor[TOPIC_BUF_LEN]       = { 0 };
static char s_topic_availability[TOPIC_BUF_LEN] = { 0 };
static char s_topic_ack[TOPIC_BUF_LEN]          = { 0 };
static char s_topic_event[TOPIC_BUF_LEN]        = { 0 };
static char s_topic_cmd[TOPIC_BUF_LEN]          = { 0 };
static char s_topic_cmd_wild[TOPIC_BUF_LEN]     = { 0 };
static char s_topic_config[TOPIC_BUF_LEN]       = { 0 };

/* 功能：留底认出回显 */
static char s_last_pub_config[512] = { 0 };
static char s_topic_get[TOPIC_BUF_LEN]          = { 0 };

/* 功能：按长度安全拷贝 */
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

/* 功能：看主题尾巴对不对 */
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

/* 功能：认出命令主题 */
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

/* 功能：解析命令 */
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
    int value = 100;                /* 功能：默认亮度 */
    int r = 255, g = 255, b = 255;  /* 功能：默认白色 */
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

    /* 功能：先拷走动作名 */
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

    /* 功能：切自动模式总开关 */
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

    /* 功能：先单独认 all */
    if (strcmp(dev, "all") == 0) {
        if (strcmp(action, MQTT_ACTION_OFF) == 0) {
            err = device_all_off(src);
        } else if (strcmp(action, MQTT_ACTION_ON) == 0) {
            /* 功能：全开只开灯和风扇 */
            /* 功能：门窗不动，怕出事 */
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

    /* 功能：取出数值参数 */
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

    /* 功能：把数夹在范围内 */
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

    /* 功能：按动作去执行 */
    if (strcmp(action, MQTT_ACTION_ON) == 0) {
        err = device_set_power(id, true, src);
    } else if (strcmp(action, MQTT_ACTION_OFF) == 0) {
        err = device_set_power(id, false, src);
    } else if (strcmp(action, MQTT_ACTION_TOGGLE) == 0) {
        err = device_toggle(id, src);
    } else if (strcmp(action, MQTT_ACTION_SET) == 0) {
        err = device_set_level(id, (uint8_t)value, src);
    } else if (strcmp(action, MQTT_ACTION_OPEN) == 0) {
        err = device_set_level(id, 100, src);      /* 功能：开就是给满值 */
    } else if (strcmp(action, MQTT_ACTION_CLOSE) == 0) {
        err = device_set_level(id, 0, src);        /* 功能：关就是给零 */
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
        /* 功能：再报一次兜底 */
        mqtt_publish_state();
    } else {
        ESP_LOGE(TAG, "cmd failed: dev=%s action=%s (%s)", dev, action, esp_err_to_name(err));
        mqtt_publish_ack(action, false, esp_err_to_name(err));
    }
}

/* 功能：解析阈值配置 */
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

    /* 功能：认出自己发的 */
    /* 功能：只对上网认回显 */
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

    /* 功能：逐项交给联动去改 */
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

/* 功能：看主题分给谁处理 */
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

/* 功能：上报一遍设备状态 */
esp_err_t mqtt_publish_state(void)
{
    /* 功能：改用堆，省栈防崩 */
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
        /* 功能：装不下就安全退出 */
        ESP_LOGE(TAG, "device_snapshot_json failed (ret=%d, buf=%u)", n, (unsigned)SNAP_BUF_LEN);
        free(snap);
        return ESP_ERR_INVALID_SIZE;
    }
    snap[n] = '\0';   /* 功能：末尾补个结尾符 */

    /* 功能：再补上网络等信息 */
    root = cJSON_Parse(snap);
    if (root == NULL) {
        ESP_LOGE(TAG, "state snapshot is not valid json");
        free(snap);
        return ESP_ERR_INVALID_ARG;
    }
    free(snap);   /* 功能：解析完就能释放 */

    (void)wifi_get_ip_str(ip, sizeof(ip));
    cJSON_AddBoolToObject(root, "auto", automation_is_enabled());
    cJSON_AddNumberToObject(root, "rssi", (double)wifi_get_rssi());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);           /* 功能：解析出来的要删 */
    if (json == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (no mem)");
        return ESP_ERR_NO_MEM;
    }

    /* 功能：两条链路都发一份 */
    app_link_broadcast(APP_MSG_STATE, json, strlen(json));
    ESP_LOGD(TAG, "state published: %s", json);
    cJSON_free(json);             /* 功能：日志打完再释放 */

    return ESP_OK;
}

/* 功能：上报一遍测量数据 */
esp_err_t mqtt_publish_sensor(const sensor_data_t *d)
{
    cJSON *root;
    char *json;

    if (d == NULL) {
        d = sensor_get_last();     /* 功能：没给就取最新 */
    }
    if (d == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 功能：字段名两边对齐 */
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

    /* 功能：两条链路都发一份 */
    app_link_broadcast(APP_MSG_SENSOR, json, strlen(json));
    cJSON_free(json);

    return ESP_OK;
}

/* 功能：上报一遍阈值设置 */
esp_err_t mqtt_publish_config(void)
{
    char buf[512];
    int n;

    n = automation_cfg_json(buf, sizeof(buf));
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        ESP_LOGE(TAG, "automation_cfg_json failed (ret=%d, buf=%u)", n, (unsigned)sizeof(buf));
        return ESP_ERR_INVALID_SIZE;
    }
    buf[n] = '\0';   /* 功能：末尾补个结尾符 */

    /* 功能：留底好认回显 */
    strncpy(s_last_pub_config, buf, sizeof(s_last_pub_config) - 1);
    s_last_pub_config[sizeof(s_last_pub_config) - 1] = '\0';

    app_link_broadcast(APP_MSG_CONFIG, buf, (size_t)n);
    ESP_LOGI(TAG, "config published: %s", buf);
    return ESP_OK;
}

/* 功能：回一条执行结果 */
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

/* 功能：报一次本地事件 */
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

    /* 功能：事件不能丢 */
    app_link_broadcast(APP_MSG_EVENT, json, strlen(json));
    cJSON_free(json);
    return ESP_OK;
}

/* 功能：看这条链路通不通 */
static bool mqtt_link_is_connected(void)
{
    return s_connected;
}

/* 功能：按类型挑主题发 */
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
        return ESP_ERR_INVALID_STATE;   /* 功能：主题还没拼好 */
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

/* 功能：看现在连上没有 */
bool mqtt_is_connected(void)
{
    return s_connected;
}

/* 功能：设备一变就上报 */
static void device_changed_cb(device_id_t id, ctrl_source_t src, void *user_data)
{
    (void)user_data;

    /* 功能：几个任务都会调 */
    /* 功能：没连上就悄悄跳过 */
    ESP_LOGD(TAG, "device changed: %s (by %s) -> publish state",
             device_id_name(id), ctrl_source_name(src));
    (void)mqtt_publish_state();
}

/* 功能：挂上状态变化回调 */
esp_err_t mqtt_app_bind_device_events(void)
{
    /* 功能：这个位置只留一个 */
    return device_register_cb(device_changed_cb, NULL);
}

/* 功能：管连接和收数据 */
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

        /* 功能：订下三条下行 */
        esp_mqtt_client_subscribe(client, s_topic_cmd_wild, 1);
        esp_mqtt_client_subscribe(client, s_topic_config, 1);
        esp_mqtt_client_subscribe(client, s_topic_get, 1);

        /* 功能：报一声我在线 */
        esp_mqtt_client_publish(client, s_topic_availability, MQTT_PAYLOAD_ONLINE, 0, 1, 1);

        /* 功能：连上先报一遍 */
        mqtt_publish_state();
        mqtt_publish_sensor(sensor_get_last());
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker, esp-mqtt will reconnect automatically");
        s_connected = false;
        led_status_set(LED_STATUS_ERROR);
        break;

    case MQTT_EVENT_DATA:
        /* 功能：按长度收数据 */
        if (event->current_data_offset != 0) {
            /* 功能：拆成几片的先不管 */
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

/* 功能：拼主题，错了就返回 */
#define BUILD_TOPIC(dst, func)                                                    \
    do {                                                                          \
        esp_err_t e_ = func((dst), sizeof(dst));                                   \
        if (e_ != ESP_OK) {                                                        \
            ESP_LOGE(TAG, "build topic " #func " failed: %s", esp_err_to_name(e_)); \
            return e_;                                                             \
        }                                                                          \
    } while (0)

/* 功能：起服务，可重复调 */
esp_err_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t cfg = { 0 };
    char uid[32] = { 0 };
    esp_err_t err;

    if (s_started) {
        ESP_LOGI(TAG, "already started, skip");
        return ESP_OK;
    }

    /* 功能：拼出各条主题 */
    BUILD_TOPIC(s_topic_state,        mqtt_topic_state);
    BUILD_TOPIC(s_topic_sensor,       mqtt_topic_sensor);
    BUILD_TOPIC(s_topic_availability, mqtt_topic_availability);
    BUILD_TOPIC(s_topic_ack,          mqtt_topic_ack);
    BUILD_TOPIC(s_topic_event,        mqtt_topic_event);
    BUILD_TOPIC(s_topic_cmd,          mqtt_topic_cmd);
    BUILD_TOPIC(s_topic_cmd_wild,     mqtt_topic_cmd_wildcard);
    BUILD_TOPIC(s_topic_config,       mqtt_topic_config);
    BUILD_TOPIC(s_topic_get,          mqtt_topic_get);

    /* 功能：客户端名必须唯一 */
    if (CONFIG_APP_MQTT_UID_OVERRIDE[0] != '\0') {
        snprintf(uid, sizeof(uid), "%s", CONFIG_APP_MQTT_UID_OVERRIDE);
    } else if (wifi_get_mac_suffix(uid, sizeof(uid)) != ESP_OK) {
        snprintf(uid, sizeof(uid), "%06x", (unsigned)(esp_random() & 0xFFFFFFu));
        ESP_LOGW(TAG, "mac unavailable, fallback uid=%s", uid);
    }
    snprintf(s_client_id, sizeof(s_client_id), "esp32sh-%s", uid);

    /* 功能：填好连接参数 */
    cfg.broker.address.uri            = CONFIG_APP_MQTT_BROKER_URI;
    cfg.credentials.client_id         = s_client_id;
    cfg.session.keepalive             = CONFIG_APP_MQTT_KEEPALIVE_S;
    cfg.session.last_will.topic       = s_topic_availability;
    cfg.session.last_will.msg         = MQTT_PAYLOAD_OFFLINE;
    cfg.session.last_will.msg_len     = (int)strlen(MQTT_PAYLOAD_OFFLINE);
    cfg.session.last_will.qos         = 1;
    cfg.session.last_will.retain      = 1;      /* 功能：这个字段是整数 */
    cfg.network.reconnect_timeout_ms  = 5000;   /* 功能：断了等五秒再连 */
    cfg.buffer.size                   = 2048;   /* 功能：给状态留够地方 */

    /* 功能：没账号就不填 */
    if (CONFIG_APP_MQTT_USERNAME[0] != '\0') {
        cfg.credentials.username = CONFIG_APP_MQTT_USERNAME;
    }
    if (CONFIG_APP_MQTT_PASSWORD[0] != '\0') {
        cfg.credentials.authentication.password = CONFIG_APP_MQTT_PASSWORD;
    }

    /* 功能：建好并启动客户端 */
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

    /* 功能：登记成上行链路 */
    /* 功能：没登记就发不出去 */
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
