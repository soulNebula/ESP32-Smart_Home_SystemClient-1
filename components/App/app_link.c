#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "app_link.h"

static const char *TAG = "app_link";

// 最多记几条路
#define APP_LINK_MAX 4

static const app_link_t *s_links[APP_LINK_MAX];
static int               s_link_cnt = 0;

// 能重进的锁，防卡死
static SemaphoreHandle_t s_lock = NULL;

static void app_link_lock_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
}

esp_err_t app_link_register(const app_link_t *link) {
    if (link == NULL || link->send == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_link_lock_init();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;

    // 已经登记过就不再记
    bool found = false;
    for (int i = 0; i < s_link_cnt; i++) {
        if (s_links[i] == link) {
            found = true;
            break;
        }
    }

    if (found) {
        ret = ESP_OK;
    } else if (s_link_cnt >= APP_LINK_MAX) {
        ESP_LOGE(TAG, "link table full (%d), cannot register \"%s\"",
                 APP_LINK_MAX, link->name ? link->name : "?");
        ret = ESP_ERR_NO_MEM;
    } else {
        s_links[s_link_cnt++] = link;
        ESP_LOGI(TAG, "link registered: \"%s\" (total %d)",
                 link->name ? link->name : "?", s_link_cnt);
    }

    xSemaphoreGiveRecursive(s_lock);
    return ret;
}

int app_link_broadcast(app_msg_type_t type, const char *json, size_t len) {
    if (json == NULL || len == 0) {
        return 0;
    }
    if (s_lock == NULL) {
        // 一条路都没登记
        return 0;
    }

    int sent = 0;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    for (int i = 0; i < s_link_cnt; i++) {
        const app_link_t *lk = s_links[i];
        if (lk == NULL || lk->send == NULL) {
            continue;
        }

        // 没连上的直接跳过
        if (lk->is_connected != NULL && !lk->is_connected()) {
            continue;
        }

        esp_err_t err = lk->send(type, json, len);
        if (err == ESP_OK) {
            sent++;
        } else if (err != ESP_ERR_INVALID_STATE) {
            // 没连上不算错，不记
            ESP_LOGW(TAG, "link \"%s\" send(%s) failed: %s",
                     lk->name ? lk->name : "?", app_msg_type_name(type),
                     esp_err_to_name(err));
        }
    }

    xSemaphoreGiveRecursive(s_lock);
    return sent;
}

const char *app_msg_type_name(app_msg_type_t type) {
    switch (type) {
    case APP_MSG_STATE:  return "state";
    case APP_MSG_SENSOR: return "sensor";
    case APP_MSG_ACK:    return "ack";
    case APP_MSG_EVENT:  return "event";
    case APP_MSG_CONFIG: return "config";
    default:             return "?";
    }
}
