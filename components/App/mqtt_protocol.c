/*
 * 模块：
 *   拼主题名。按配置里的前缀加这台板的号，拼出收发要用的各条主题，
 *   给 mqtt_app.c 调用。板的号取法和 ble_app.c 那边一样，
 *   为的是一台板子在两条链路上被认成同一台。
 *
 * 功能：
 *   取这台板的号
 *   拼出各条主题
 *   拼不下就报错
 */
#include "mqtt_protocol.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#include "wifi_sta.h"

static const char *TAG = "mqtt_protocol";

/* 功能：放板子的号 */
#define MQTT_UID_MAX   32
/* 功能：放前缀加号 */
#define MQTT_BASE_MAX  64

/* 功能：取这台板的号 */
static esp_err_t build_uid(char *buf, size_t len)
{
    int n;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < 7) {                      /* 功能：号加结尾符 */
        return ESP_ERR_INVALID_SIZE;
    }

    if (CONFIG_APP_MQTT_UID_OVERRIDE[0] != '\0') {
        n = snprintf(buf, len, "%s", CONFIG_APP_MQTT_UID_OVERRIDE);
        if (n < 0 || (size_t)n >= len) {
            ESP_LOGE(TAG, "CONFIG_APP_MQTT_UID_OVERRIDE too long for %u bytes",
                     (unsigned)len);
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    return wifi_get_mac_suffix(buf, len);
}

/* 功能：拼出前缀加号 */
static esp_err_t build_base(char *buf, size_t len)
{
    char uid[MQTT_UID_MAX] = { 0 };
    esp_err_t err;
    int n;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_uid(uid, sizeof(uid));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build uid failed: %s", esp_err_to_name(err));
        return err;
    }

    n = snprintf(buf, len, "%s/%s", CONFIG_APP_MQTT_TOPIC_PREFIX, uid);
    if (n < 0 || (size_t)n >= len) {
        ESP_LOGE(TAG, "base topic truncated (%d >= %u)", n, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* 功能：前缀号再接尾巴 */
static esp_err_t build_topic(char *buf, size_t len, const char *suffix)
{
    char base[MQTT_BASE_MAX] = { 0 };
    esp_err_t err;
    int n;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_base(base, sizeof(base));
    if (err != ESP_OK) {
        return err;
    }

    if (suffix == NULL) {
        n = snprintf(buf, len, "%s", base);
    } else {
        n = snprintf(buf, len, "%s%s", base, suffix);
    }

    if (n < 0 || (size_t)n >= len) {
        ESP_LOGE(TAG, "topic \"%s%s\" needs %d bytes, only %u available",
                 base, (suffix != NULL) ? suffix : "", n, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* 功能：拼出基础主题 */
esp_err_t mqtt_topic_base(char *buf, size_t len)
{
    return build_topic(buf, len, NULL);
}

/* 功能：拼出状态主题 */
esp_err_t mqtt_topic_state(char *buf, size_t len)
{
    return build_topic(buf, len, "/state");
}

/* 功能：拼出测量主题 */
esp_err_t mqtt_topic_sensor(char *buf, size_t len)
{
    return build_topic(buf, len, "/sensor");
}

/* 功能：拼出在线主题 */
esp_err_t mqtt_topic_availability(char *buf, size_t len)
{
    return build_topic(buf, len, "/availability");
}

/* 功能：拼出回执主题 */
esp_err_t mqtt_topic_ack(char *buf, size_t len)
{
    return build_topic(buf, len, "/ack");
}

/* 功能：拼出事件主题 */
esp_err_t mqtt_topic_event(char *buf, size_t len)
{
    return build_topic(buf, len, "/event");
}

/* 功能：拼出命令主题 */
esp_err_t mqtt_topic_cmd(char *buf, size_t len)
{
    return build_topic(buf, len, "/cmd");
}

/* 功能：拼出命令通配主题 */
esp_err_t mqtt_topic_cmd_wildcard(char *buf, size_t len)
{
    return build_topic(buf, len, "/cmd/#");
}

/* 功能：拼出阈值主题 */
esp_err_t mqtt_topic_config(char *buf, size_t len)
{
    return build_topic(buf, len, "/config");
}

/* 功能：拼出查询主题 */
esp_err_t mqtt_topic_get(char *buf, size_t len)
{
    return build_topic(buf, len, "/get");
}
