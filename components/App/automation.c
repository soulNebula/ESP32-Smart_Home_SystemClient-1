#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

// 看自动模式的出厂默认
#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "automation.h"
#include "device_model.h"
#include "board_config.h"
#include "sensor.h"

static const char *TAG = "automation";

#define AUTO_NVS_NAMESPACE  "sh_auto"
#define AUTO_NVS_KEY        "cfg"

// 四个字母拼成记号
#define AUTO_CFG_MAGIC      ((uint32_t)('A') | ((uint32_t)('U') << 8) | \
                             ((uint32_t)('T') << 16) | ((uint32_t)('O') << 24))
#define AUTO_CFG_VERSION    1

// 存到 flash 里的整包
typedef struct {
    // 记号，认是不是自己的
    uint32_t magic;
    // 第几版，好认旧数据
    uint16_t version;
    // 配置多大，对一下
    uint16_t cfg_size;
    // 真正那几条线
    automation_cfg_t cfg;
} auto_cfg_blob_t;

// 头固定八字节
_Static_assert(offsetof(auto_cfg_blob_t, cfg) == 8,
               "auto_cfg_blob_t header must stay exactly 8 bytes");
// 配置别太大，装得下
_Static_assert(sizeof(automation_cfg_t) < 65535, "automation_cfg_t too large for the blob header");

// 出厂默认开不开自动
#ifdef CONFIG_APP_AUTO_ENABLE_DEFAULT
#define AUTO_DEF_ENABLED    true
#else
#define AUTO_DEF_ENABLED    false
#endif

// 一次写好默认那几条线
#define AUTO_CFG_DEFAULT_INIT {                    \
    .enabled            = AUTO_DEF_ENABLED,        \
    .light_on_lux       = (float)BSP_DEF_LIGHT_ON_LUX,   /* 默认五十 */   \
    .light_off_lux      = (float)BSP_DEF_LIGHT_OFF_LUX,  /* 默认两百 */  \
    .temp_fan_on_c      = (float)BSP_DEF_TEMP_FAN_ON_C,  /* 默认二十八 */   \
    .temp_fan_off_c     = (float)BSP_DEF_TEMP_FAN_OFF_C, /* 默认二十六 */   \
    .fan_auto_speed     = 70,                      \
    .rain_pct           = (float)BSP_DEF_RAIN_PCT, /* 默认三十 */   \
    .auto_window_reopen = false,                   /* 雨停先不开窗 */ \
    .auto_light_enable  = true,                    \
    .auto_temp_enable   = true,                    \
    .auto_rain_enable   = true,                    \
}

// 眼下用的那几条线
static automation_cfg_t s_cfg = AUTO_CFG_DEFAULT_INIT;

// 记下每台人动过的时间
static int64_t s_last_manual_ms[DEV_COUNT];

static SemaphoreHandle_t s_lock  = NULL;
static bool              s_inited = false;

static inline void auto_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void auto_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void cfg_set_defaults(automation_cfg_t *c)
{
    *c = (automation_cfg_t)AUTO_CFG_DEFAULT_INIT;
}

static void cfg_store(const automation_cfg_t *c)
{
    auto_lock();
    s_cfg = *c;
    auto_unlock();
}

// 锁里抄一份配置出来
static void cfg_snapshot(automation_cfg_t *out)
{
    auto_lock();
    *out = s_cfg;
    auto_unlock();
}

// 查查这几条线合不合理
static bool cfg_validate(const automation_cfg_t *c)
{
    if (!isfinite(c->light_on_lux) || !isfinite(c->light_off_lux) ||
        !isfinite(c->temp_fan_on_c) || !isfinite(c->temp_fan_off_c) ||
        !isfinite(c->rain_pct)) {
        return false;
    }
    if (c->light_on_lux <= 0.0f) {
        // 开灯线得是正数
        return false;
    }
    if (c->light_off_lux <= c->light_on_lux) {
        // 两条线高低写反了
        return false;
    }
    if ((c->temp_fan_on_c < -10.0f) || (c->temp_fan_on_c > 60.0f)) {
        return false;
    }
    if (c->temp_fan_off_c >= c->temp_fan_on_c) {
        // 关风扇的线得低些
        return false;
    }
    if ((c->rain_pct < 0.0f) || (c->rain_pct > 100.0f)) {
        return false;
    }
    if (c->fan_auto_speed > 100) {
        return false;
    }
    return true;
}

esp_err_t automation_init(void)
{
    if (s_inited) {
        ESP_LOGD(TAG, "already initialized");
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGW(TAG, "mutex create failed, run without lock");
        }
    }

    // 存东西的地方先备好
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(TAG, "nvs_flash_init: %s -> erase & retry", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init failed: %s (auto cfg will use defaults)",
                 esp_err_to_name(err));
    }

    // 读不到就用默认值
    err = automation_load();
    s_inited = true;

    ESP_LOGI(TAG, "automation init done (%s)",
             (err == ESP_OK) ? "cfg loaded from NVS" : "using default cfg");

    // 留够地方拼 JSON
    char buf[320];
    if (automation_cfg_json(buf, sizeof(buf)) > 0) {
        ESP_LOGI(TAG, "cfg = %s", buf);
    }

    // 配置坏了也别起不来
    return ESP_OK;
}

esp_err_t automation_load(void)
{
    automation_cfg_t loaded;
    cfg_set_defaults(&loaded);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(AUTO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 头回上电没存过，正常
        ESP_LOGI(TAG, "nvs_open(%s) -> %s, use defaults",
                 AUTO_NVS_NAMESPACE, esp_err_to_name(err));
        cfg_store(&loaded);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    auto_cfg_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, AUTO_NVS_KEY, &blob, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved cfg, use defaults");
        cfg_store(&loaded);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob failed: %s, use defaults", esp_err_to_name(err));
        cfg_store(&loaded);
        return err;
    }
    if (size != sizeof(blob)) {
        ESP_LOGW(TAG, "cfg blob size %u != %u, discard", (unsigned)size, (unsigned)sizeof(blob));
        cfg_store(&loaded);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((blob.magic != AUTO_CFG_MAGIC) || (blob.version != AUTO_CFG_VERSION)) {
        ESP_LOGW(TAG, "cfg magic/version mismatch (0x%08x v%u), discard",
                 (unsigned)blob.magic, (unsigned)blob.version);
        cfg_store(&loaded);
        return ESP_ERR_INVALID_VERSION;
    }
    if ((blob.cfg_size != sizeof(automation_cfg_t)) || !cfg_validate(&blob.cfg)) {
        ESP_LOGW(TAG, "cfg body invalid (size %u), discard", (unsigned)blob.cfg_size);
        cfg_store(&loaded);
        return ESP_ERR_INVALID_STATE;
    }

    cfg_store(&blob.cfg);
    ESP_LOGI(TAG, "cfg loaded from NVS");
    return ESP_OK;
}

esp_err_t automation_save(void)
{
    auto_cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));

    blob.magic    = AUTO_CFG_MAGIC;
    blob.version  = AUTO_CFG_VERSION;
    blob.cfg_size = (uint16_t)sizeof(automation_cfg_t);
    cfg_snapshot(&blob.cfg);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(AUTO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", AUTO_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, AUTO_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        // 不落盘掉电就丢
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cfg save failed: %s", esp_err_to_name(err));
        return err;
    }

    char buf[320];
    if (automation_cfg_json(buf, sizeof(buf)) > 0) {
        ESP_LOGI(TAG, "cfg saved: %s", buf);
    }
    return ESP_OK;
}

esp_err_t automation_set_enabled(bool enabled)
{
    auto_lock();
    s_cfg.enabled = enabled;
    auto_unlock();

    ESP_LOGI(TAG, "auto mode %s", enabled ? "ON" : "OFF");
    return ESP_OK;
}

bool automation_is_enabled(void)
{
    auto_lock();
    bool en = s_cfg.enabled;
    auto_unlock();
    return en;
}

// 给的是里头地址，改完存
automation_cfg_t *automation_get_cfg(void)
{
    return &s_cfg;
}

esp_err_t automation_set_threshold(const char *key, float value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    bool      log_bool = false;
    bool      bool_val = false;

    // 不像数的值直接挡掉
    if (!isfinite(value)) {
        ESP_LOGW(TAG, "cfg.%s: non-finite value rejected", key);
        return ESP_ERR_INVALID_SIZE;
    }

    auto_lock();

    if (strcmp(key, "enabled") == 0) {
        s_cfg.enabled = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.enabled;
    } else if (strcmp(key, "light_on_lux") == 0) {
        // 开灯线必须比关灯线低
        if (value <= 0.0f || value >= s_cfg.light_off_lux) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.light_on_lux = value;
        }
    } else if (strcmp(key, "light_off_lux") == 0) {
        // 两条线中间那段先不动
        if (value <= s_cfg.light_on_lux) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.light_off_lux = value;
        }
    } else if (strcmp(key, "temp_fan_on_c") == 0) {
        // 开风扇线必须比关的高
        if ((value < -10.0f) || (value > 60.0f) || value <= s_cfg.temp_fan_off_c) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.temp_fan_on_c = value;
        }
    } else if (strcmp(key, "temp_fan_off_c") == 0) {
        // 关风扇线得低一些
        if (value >= s_cfg.temp_fan_on_c) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.temp_fan_off_c = value;
        }
    } else if (strcmp(key, "rain_pct") == 0) {
        if ((value < 0.0f) || (value > 100.0f)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.rain_pct = value;
        }
    } else if (strcmp(key, "fan_auto_speed") == 0) {
        if ((value < 0.0f) || (value > 100.0f)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            // 四舍五入成整数
            s_cfg.fan_auto_speed = (uint8_t)(value + 0.5f);
        }
    } else if (strcmp(key, "auto_light_enable") == 0) {
        s_cfg.auto_light_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_light_enable;
    } else if (strcmp(key, "auto_temp_enable") == 0) {
        s_cfg.auto_temp_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_temp_enable;
    } else if (strcmp(key, "auto_rain_enable") == 0) {
        s_cfg.auto_rain_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_rain_enable;
    } else if (strcmp(key, "auto_window_reopen") == 0) {
        s_cfg.auto_window_reopen = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_window_reopen;
    } else {
        // 这个名字不认识
        err = ESP_ERR_INVALID_ARG;
    }

    auto_unlock();

    // 出了锁再打日志
    if (err == ESP_OK) {
        if (log_bool) {
            ESP_LOGI(TAG, "cfg.%s = %s", key, bool_val ? "true" : "false");
        } else {
            ESP_LOGI(TAG, "cfg.%s = %.2f", key, (double)value);
        }
    } else {
        ESP_LOGW(TAG, "cfg.%s = %.2f rejected: %s", key, (double)value, esp_err_to_name(err));
    }

    // 这儿先不存，等外面喊存
    return err;
}

// 拿一份配置拼成 JSON
static int cfg_json_build(const automation_cfg_t *c, char *buf, size_t len)
{
    if ((c == NULL) || (buf == NULL) || (len == 0)) {
        return 0;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cfg_json: cJSON_CreateObject failed");
        return 0;
    }

    (void)cJSON_AddBoolToObject(root, "enabled", c->enabled);
    (void)cJSON_AddNumberToObject(root, "light_on_lux", (double)c->light_on_lux);
    (void)cJSON_AddNumberToObject(root, "light_off_lux", (double)c->light_off_lux);
    (void)cJSON_AddNumberToObject(root, "temp_fan_on_c", (double)c->temp_fan_on_c);
    (void)cJSON_AddNumberToObject(root, "temp_fan_off_c", (double)c->temp_fan_off_c);
    (void)cJSON_AddNumberToObject(root, "rain_pct", (double)c->rain_pct);
    (void)cJSON_AddNumberToObject(root, "fan_auto_speed", (double)c->fan_auto_speed);
    (void)cJSON_AddBoolToObject(root, "auto_light_enable", c->auto_light_enable);
    (void)cJSON_AddBoolToObject(root, "auto_temp_enable", c->auto_temp_enable);
    (void)cJSON_AddBoolToObject(root, "auto_rain_enable", c->auto_rain_enable);
    (void)cJSON_AddBoolToObject(root, "auto_window_reopen", c->auto_window_reopen);

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js == NULL) {
        ESP_LOGE(TAG, "cfg_json: print failed");
        return 0;
    }

    size_t need = strlen(js) + 1;
    if (need > len) {
        ESP_LOGW(TAG, "cfg_json buffer too small: need %u, have %u", (unsigned)need, (unsigned)len);
        cJSON_free(js);
        return 0;
    }

    memcpy(buf, js, need);
    int written = (int)(need - 1);
    cJSON_free(js);
    return written;
}

int automation_cfg_json(char *buf, size_t len)
{
    automation_cfg_t snap;
    cfg_snapshot(&snap);
    return cfg_json_build(&snap, buf, len);
}

void automation_notify_manual(device_id_t id)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;

    auto_lock();
    if (id == DEV_COUNT) {
        // 表尾这个值代表全部
        for (int i = 0; i < DEV_COUNT; i++) {
            s_last_manual_ms[i] = now_ms;
        }
    } else if (((int)id >= 0) && (id < DEV_COUNT)) {
        s_last_manual_ms[id] = now_ms;
    }
    auto_unlock();

    ESP_LOGD(TAG, "manual guard start on %s",
             (id == DEV_COUNT) ? "all" : device_id_name(id));
}

// 看这台现在让不让碰
static bool guard_ok(device_id_t id, int64_t now_ms)
{
    if (((int)id < 0) || (id >= DEV_COUNT)) {
        return true;
    }

    int64_t last = s_last_manual_ms[id];
    if (last == 0) {
        // 没人动过，随便调
        return true;
    }
    return (now_ms - last) >= (int64_t)AUTO_MANUAL_GUARD_MS;
}

// 看光照的数是不是真的
static bool light_data_valid(const sensor_data_t *d)
{
    return d->light_is_bh1750 || (d->light_mv > 0) || (d->light_pct > 0.0f);
}

void automation_tick(const sensor_data_t *d)
{
    if (d == NULL) {
        return;
    }

    automation_cfg_t cfg;
    cfg_snapshot(&cfg);

    if (!cfg.enabled) {
        ESP_LOGD(TAG, "auto mode disabled, skip");
        return;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;

    // 头一条，看光照暗不暗
    if (cfg.auto_light_enable && light_data_valid(d)) {
        // 客厅那盏灯
        if (!device_get_power(DEV_LED_LIVING) && (d->lux < cfg.light_on_lux)) {
            if (guard_ok(DEV_LED_LIVING, now_ms)) {
                ESP_LOGI(TAG, "rule1: lux %.1f < on %.1f & living LED off -> LED ON",
                         (double)d->lux, (double)cfg.light_on_lux);
                device_set_power(DEV_LED_LIVING, true, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule1: living LED in manual guard, skip");
            }
        } else if (device_get_power(DEV_LED_LIVING) && (d->lux > cfg.light_off_lux)) {
            if (guard_ok(DEV_LED_LIVING, now_ms)) {
                ESP_LOGI(TAG, "rule1: lux %.1f > off %.1f & living LED on -> LED OFF",
                         (double)d->lux, (double)cfg.light_off_lux);
                device_set_power(DEV_LED_LIVING, false, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule1: living LED in manual guard, skip");
            }
        }

        // 窗帘和灯正相反
        const uint8_t curtain_pos = device_get_level(DEV_CURTAIN);
        if ((d->lux < cfg.light_on_lux) && (curtain_pos > 0)) {
            if (guard_ok(DEV_CURTAIN, now_ms)) {
                ESP_LOGI(TAG, "rule1: lux %.1f < on %.1f & curtain pos %u -> CURTAIN CLOSE(0)",
                         (double)d->lux, (double)cfg.light_on_lux, (unsigned)curtain_pos);
                device_set_level(DEV_CURTAIN, 0, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule1: curtain in manual guard, skip");
            }
        } else if ((d->lux > cfg.light_off_lux) && (curtain_pos < 100)) {
            if (guard_ok(DEV_CURTAIN, now_ms)) {
                ESP_LOGI(TAG, "rule1: lux %.1f > off %.1f & curtain pos %u -> CURTAIN OPEN(100)",
                         (double)d->lux, (double)cfg.light_off_lux, (unsigned)curtain_pos);
                device_set_level(DEV_CURTAIN, 100, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule1: curtain in manual guard, skip");
            }
        }
    }

    // 热了开风扇，凉了关
    if (cfg.auto_temp_enable && d->valid_temp) {
        if (!device_get_power(DEV_FAN) && (d->temperature > cfg.temp_fan_on_c)) {
            if (guard_ok(DEV_FAN, now_ms)) {
                ESP_LOGI(TAG, "rule2: temp %.1f > on %.1f & fan off -> FAN ON %u%%",
                         (double)d->temperature, (double)cfg.temp_fan_on_c,
                         (unsigned)cfg.fan_auto_speed);
                device_set_level(DEV_FAN, cfg.fan_auto_speed, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule2: fan in manual guard, skip");
            }
        } else if (device_get_power(DEV_FAN) && (d->temperature < cfg.temp_fan_off_c)) {
            if (guard_ok(DEV_FAN, now_ms)) {
                ESP_LOGI(TAG, "rule2: temp %.1f < off %.1f & fan on -> FAN OFF",
                         (double)d->temperature, (double)cfg.temp_fan_off_c);
                device_set_power(DEV_FAN, false, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule2: fan in manual guard, skip");
            }
        }
    }

    // 下雨就把窗关上
    if (cfg.auto_rain_enable) {
        const bool raining     = (d->rain_pct > cfg.rain_pct);
        const bool window_open = device_get_power(DEV_WINDOW);

        if (raining && window_open) {
            if (guard_ok(DEV_WINDOW, now_ms)) {
                ESP_LOGI(TAG, "rule3: rain %.1f%% > %.1f%% & window open -> WINDOW CLOSE",
                         (double)d->rain_pct, (double)cfg.rain_pct);
                device_set_power(DEV_WINDOW, false, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule3: window in manual guard, skip");
            }
        } else if (!raining && !window_open && cfg.auto_window_reopen) {
            if (guard_ok(DEV_WINDOW, now_ms)) {
                ESP_LOGI(TAG, "rule3: rain %.1f%% <= %.1f%% & window closed -> WINDOW REOPEN",
                         (double)d->rain_pct, (double)cfg.rain_pct);
                device_set_power(DEV_WINDOW, true, SRC_AUTO);
            } else {
                ESP_LOGD(TAG, "rule3: window in manual guard, skip");
            }
        }
    }
}
