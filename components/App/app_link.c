/**
 * @file  app_link.c
 * @brief 传输链路注册表实现（见 app_link.h 的设计说明）
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "app_link.h"

static const char *TAG = "app_link";

/* 最多几条链路：MQTT + BLE 是 2 条，留点余量给以后（WebSocket / USB 串口） */
#define APP_LINK_MAX 4

static const app_link_t *s_links[APP_LINK_MAX];
static int               s_link_cnt = 0;
static bool              s_inited   = false;

/* 用【递归】互斥锁：广播时持锁调用链路 send()，而 send() 有可能间接触发
 * 另一次广播（例如 BLE notify 后又引起状态变化）。递归锁允许同任务重入，
 * 普通互斥锁在这种路径上会直接死锁。 */
static SemaphoreHandle_t s_lock = NULL;

static void app_link_lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
}

esp_err_t app_link_register(const app_link_t *link)
{
    if (link == NULL || link->send == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_link_lock_init();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;

    /* 已注册过（幂等）：比较指针，避免重复 init 时塞两条一样的 */
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
        s_inited = true;
        ESP_LOGI(TAG, "link registered: \"%s\" (total %d)",
                 link->name ? link->name : "?", s_link_cnt);
    }

    xSemaphoreGiveRecursive(s_lock);
    return ret;
}

esp_err_t app_link_unregister(const app_link_t *link)
{
    if (link == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_link_cnt; i++) {
        if (s_links[i] == link) {
            /* 后面的往前挪，保持数组紧凑 */
            for (int j = i; j < s_link_cnt - 1; j++) {
                s_links[j] = s_links[j + 1];
            }
            s_links[--s_link_cnt] = NULL;
            ESP_LOGI(TAG, "link unregistered: \"%s\" (left %d)",
                     link->name ? link->name : "?", s_link_cnt);
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGiveRecursive(s_lock);
    return ret;
}

int app_link_broadcast(app_msg_type_t type, const char *json, size_t len)
{
    if (json == NULL || len == 0) {
        return 0;
    }
    if (s_lock == NULL) {
        return 0;   /* 一条链路都没注册过 */
    }

    int sent = 0;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);

    for (int i = 0; i < s_link_cnt; i++) {
        const app_link_t *lk = s_links[i];
        if (lk == NULL || lk->send == NULL) {
            continue;
        }

        /* 未连接的链路直接跳过 —— 这样 MQTT 断线时不会刷一堆失败日志 */
        if (lk->is_connected != NULL && !lk->is_connected()) {
            continue;
        }

        esp_err_t err = lk->send(type, json, len);
        if (err == ESP_OK) {
            sent++;
        } else if (err != ESP_ERR_INVALID_STATE) {
            /* INVALID_STATE 是"这轮没连上"，属于常态，不刷日志 */
            ESP_LOGW(TAG, "link \"%s\" send(%s) failed: %s",
                     lk->name ? lk->name : "?", app_msg_type_name(type),
                     esp_err_to_name(err));
        }
    }

    xSemaphoreGiveRecursive(s_lock);
    return sent;
}

bool app_link_any_connected(void)
{
    if (s_lock == NULL) {
        return false;
    }

    bool any = false;

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_link_cnt; i++) {
        const app_link_t *lk = s_links[i];
        if (lk != NULL && lk->is_connected != NULL && lk->is_connected()) {
            any = true;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_lock);

    (void)s_inited;
    return any;
}

const char *app_msg_type_name(app_msg_type_t type)
{
    switch (type) {
    case APP_MSG_STATE:  return "state";
    case APP_MSG_SENSOR: return "sensor";
    case APP_MSG_ACK:    return "ack";
    case APP_MSG_EVENT:  return "event";
    case APP_MSG_CONFIG: return "config";
    default:             return "?";
    }
}
