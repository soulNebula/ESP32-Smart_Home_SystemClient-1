/**
 * @file  wifi_sta.c
 * @brief WiFi Station 连接管理实现（对应 wifi_sta.h）
 *
 * 设计要点：
 *   · wifi_init_sta() 全程非阻塞：注册完事件回调、start 之后就返回，
 *     真正的"连上没连上"由事件回调更新状态，用 wifi_is_connected() 查询。
 *   · 幂等：重复调用只做一次初始化，第二次直接返回 ESP_OK。
 *   · 断线重连策略由 CONFIG_APP_WIFI_MAX_RETRY 决定：
 *       0    = 永不放弃（推荐，产品模式）
 *       > 0  = 连续失败超过该次数就 esp_restart()（现场自愈）
 */

#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "led.h"

static const char *TAG = "wifi_sta";

/* 等待连接的轮询粒度：100ms（需求指定） */
#define WIFI_WAIT_POLL_MS   100
/* "0.0.0.0" + '\0' 够用，留点余量 */
#define WIFI_IP_STR_LEN     16

/* ---------------- 模块状态 ---------------- */
static bool                     s_inited   = false;   /* 初始化幂等标志 */
static volatile bool            s_got_ip   = false;   /* 是否已拿到 IP */
static char                     s_ip_str[WIFI_IP_STR_LEN] = "0.0.0.0";
static int                      s_retry    = 0;       /* 连续重连次数 */
static esp_netif_t             *s_netif    = NULL;
static esp_event_handler_instance_t s_wifi_evt_inst = NULL;
static esp_event_handler_instance_t s_ip_evt_inst   = NULL;

/* ---------------- 内部小工具 ---------------- */

/** @brief 把 src 拷进定长 uint8_t 缓冲（wifi_config_t 的字段是 uint8_t[]），保证 '\0' 结尾 */
static void copy_into_u8(uint8_t *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy((char *)dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* ---------------- 事件回调 ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting to \"%s\" ...", CONFIG_APP_WIFI_SSID);
            led_status_set(LED_STATUS_WIFI_CONNECTING);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            uint8_t reason = (disc != NULL) ? disc->reason : 0;

            s_got_ip = false;
            snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
            led_status_set(LED_STATUS_WIFI_CONNECTING);

            if (CONFIG_APP_WIFI_MAX_RETRY == 0) {
                /* 0 = 永不放弃 */
                ESP_LOGW(TAG, "disconnected (reason=%u), reconnecting forever ...", reason);
                esp_wifi_connect();
            } else {
                s_retry++;
                if (s_retry > CONFIG_APP_WIFI_MAX_RETRY) {
                    ESP_LOGE(TAG, "disconnected (reason=%u), retry %d > %d, restarting ...",
                             reason, s_retry - 1, CONFIG_APP_WIFI_MAX_RETRY);
                    esp_restart();
                }
                ESP_LOGW(TAG, "disconnected (reason=%u), retry %d/%d ...",
                         reason, s_retry, CONFIG_APP_WIFI_MAX_RETRY);
                esp_wifi_connect();
            }
            break;
        }

        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = (ip_event_got_ip_t *)event_data;

        if (got != NULL) {
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&got->ip_info.ip));
        }
        s_got_ip = true;
        s_retry  = 0;
        led_status_set(LED_STATUS_WIFI_OK);
        ESP_LOGI(TAG, "got ip: %s", s_ip_str);
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t wifi_init_sta(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized, skip");
        return ESP_OK;
    }

    esp_err_t err;

    /* WiFi 协议栈要往 NVS 里存 PHY 校准数据，必须先 init。
     * 这里再调一次是幂等的（已初始化会直接返回 ESP_OK），
     * 免得依赖 main.c 的调用顺序。 */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init: %s, erasing and retrying", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    /* 事件循环可能已经被别的模块建好了，重复创建会返回 ESP_ERR_INVALID_STATE，
     * 那种情况不算错误。 */
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_netif == NULL) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* 注册事件：WIFI_EVENT 管连接/断开，IP_EVENT 管拿到 IP */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, &s_ip_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_cfg = { 0 };
    copy_into_u8(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), CONFIG_APP_WIFI_SSID);
    copy_into_u8(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), CONFIG_APP_WIFI_PASSWORD);

    /* 开放网络（无密码）必须把门槛设成 OPEN，否则 WPA2 门槛扫描不到它 */
    if (CONFIG_APP_WIFI_PASSWORD[0] == '\0') {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    /* esp_wifi_set_config() 的形参是 wifi_config_t*（不是 const），传 &wifi_cfg 即可 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    ESP_LOGI(TAG, "init done: ssid=\"%s\" max_retry=%d", CONFIG_APP_WIFI_SSID,
             CONFIG_APP_WIFI_MAX_RETRY);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_got_ip;
}

void wifi_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (!s_got_ip && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_WAIT_POLL_MS));
        waited += WIFI_WAIT_POLL_MS;
    }

    if (s_got_ip) {
        ESP_LOGI(TAG, "connected, ip=%s", s_ip_str);
    } else {
        ESP_LOGW(TAG, "wait connected timeout (%u ms), keep running offline",
                 (unsigned)timeout_ms);
    }
}

esp_err_t wifi_get_ip_str(char *buf, size_t len)
{
    int n;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    n = snprintf(buf, len, "%s", s_got_ip ? s_ip_str : "0.0.0.0");
    if (n < 0 || (size_t)n >= len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

int8_t wifi_get_rssi(void)
{
    wifi_ap_record_t ap = { 0 };

    if (!s_got_ip) {
        return 0;
    }
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t wifi_get_mac_suffix(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_err_t err;

    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 6 位十六进制 + '\0' = 7，len 至少 7 */
    if (len < 7) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        /* WiFi 还没 init 时会走到这里：退回直接读 eFuse，保证唯一 ID 一定拿得到 */
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read mac failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    snprintf(buf, len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}
