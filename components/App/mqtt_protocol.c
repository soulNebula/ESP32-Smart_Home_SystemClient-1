/**
 * @file  mqtt_protocol.c
 * @brief MQTT topic 拼装实现（topic 规划见 mqtt_protocol.h）
 *
 * base = <CONFIG_APP_MQTT_TOPIC_PREFIX>/<唯一ID>
 * 唯一ID：CONFIG_APP_MQTT_UID_OVERRIDE 非空则用它，否则用 MAC 后 3 字节（如 "a1b2c3"）。
 *
 * 所有拼接都是 snprintf + 截断检查：
 *   snprintf 返回值 >= len 说明被截断了 → ESP_ERR_INVALID_SIZE，
 *   宁可返回错误也不发一个半截的 topic 出去。
 */

#include "mqtt_protocol.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"

#include "wifi_sta.h"

static const char *TAG = "mqtt_protocol";

/* 唯一 ID 缓冲：MAC 后缀 6 位，覆盖值理论上更长，给 32 足够 */
#define MQTT_UID_MAX   32
/* base 缓冲：前缀（menuconfig，通常 <16）+ '/' + UID */
#define MQTT_BASE_MAX  64

/**
 * @brief 取唯一 ID（"a1b2c3" 之类）
 * @note  CONFIG_APP_MQTT_UID_OVERRIDE 是字符串宏，判断"有没有配"必须看首字符，
 *        不能只看宏是否定义 —— 它在 Kconfig 里默认就是空串。
 */
static esp_err_t build_uid(char *buf, size_t len)
{
    int n;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < 7) {                      /* "a1b2c3" + '\0' */
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

/**
 * @brief 拼 base = "<prefix>/<uid>"
 */
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

/**
 * @brief base + suffix 的通用拼接；suffix 为 NULL 时就是 base 本身
 */
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

/* ---------------- 对外接口 ---------------- */

esp_err_t mqtt_topic_base(char *buf, size_t len)
{
    return build_topic(buf, len, NULL);
}

esp_err_t mqtt_topic_state(char *buf, size_t len)
{
    return build_topic(buf, len, "/state");
}

esp_err_t mqtt_topic_sensor(char *buf, size_t len)
{
    return build_topic(buf, len, "/sensor");
}

esp_err_t mqtt_topic_availability(char *buf, size_t len)
{
    return build_topic(buf, len, "/availability");
}

esp_err_t mqtt_topic_ack(char *buf, size_t len)
{
    return build_topic(buf, len, "/ack");
}

esp_err_t mqtt_topic_event(char *buf, size_t len)
{
    return build_topic(buf, len, "/event");
}

esp_err_t mqtt_topic_cmd(char *buf, size_t len)
{
    return build_topic(buf, len, "/cmd");
}

esp_err_t mqtt_topic_cmd_wildcard(char *buf, size_t len)
{
    return build_topic(buf, len, "/cmd/#");
}

esp_err_t mqtt_topic_config(char *buf, size_t len)
{
    return build_topic(buf, len, "/config");
}

esp_err_t mqtt_topic_get(char *buf, size_t len)
{
    return build_topic(buf, len, "/get");
}
