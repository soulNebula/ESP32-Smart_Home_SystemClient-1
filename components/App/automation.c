/**
 * @file  automation.c
 * @brief 自动联动规则引擎实现 —— 对应需求 1（光照）/ 2（温度）/ 3（雨滴）
 *
 * 规则总览（全部可通过 MQTT config topic 在运行时改阈值，无需重新烧录）：
 *   规则1 光照：lux < light_on_lux  → 开客厅灯 + 合上窗帘（遮光/保温）
 *               lux > light_off_lux → 关客厅灯 + 拉开窗帘（自然采光）
 *               ★ 灯和窗帘的动作方向【相反】：暗了要开灯，但窗帘应该拉上。
 *   规则2 温度：temperature > temp_fan_on_c  → 开风扇（fan_auto_speed%）
 *               temperature < temp_fan_off_c → 关风扇
 *   规则3 雨滴：rain_pct > rain_pct 阈值 → 判为下雨 → 关窗
 *               雨停且 auto_window_reopen → 重新开窗
 *
 * 两条通用保护：
 *   ① 迟滞（hysteresis）：开/关用两个阈值，中间区间保持不动，避免阈值附近反复横跳；
 *   ② 手动优先：设备在 AUTO_MANUAL_GUARD_MS 内被手动操作过（key/voice/mqtt），
 *      本设备相关的规则直接跳过，不覆盖用户的即时操作。
 *
 * 【线程与阻塞】
 *   automation_tick 由主循环（app_main 任务）周期性调用；automation_set_threshold /
 *   automation_save 由 MQTT 任务调用 —— 用一把互斥锁保护 s_cfg 与 s_last_manual_ms。
 *   tick 里【不做任何阻塞操作】，全部通过 device_model 的 set 接口下发动作，
 *   舵机走 servo_set_percent（BSP 内部直接改 LEDC 占空比，立即返回），
 *   绝不使用 servo_set_angle_smooth（那是 delay 式分步插值，会阻塞主循环）。
 */
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "sdkconfig.h"      /* CONFIG_APP_AUTO_ENABLE_DEFAULT */

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

/* ========================================================================= */
/*  NVS 持久化：带 magic + version 的 blob，方便以后加字段                    */
/* ========================================================================= */
#define AUTO_NVS_NAMESPACE  "sh_auto"
#define AUTO_NVS_KEY        "cfg"

/* 'A','U','T','O' 按内存顺序（小端）组成一个 uint32 */
#define AUTO_CFG_MAGIC      ((uint32_t)('A') | ((uint32_t)('U') << 8) | \
                             ((uint32_t)('T') << 16) | ((uint32_t)('O') << 24))
#define AUTO_CFG_VERSION    1

/**
 * @brief NVS 里实际存的东西
 *
 * 为什么不直接存 automation_cfg_t：
 *   结构体可能有 padding，而且以后一定会加字段。存一个"头 + 配置体"，
 *   读的时候先校验 magic / version / 尺寸，不匹配就整块丢弃用默认值。
 *   这样旧固件写的数据不会被新固件误读成乱码（宁可回到默认值，也不要读到垃圾）。
 *   以后加字段只把 AUTO_CFG_VERSION +1，并在 load 里做迁移即可。
 */
typedef struct {
    uint32_t magic;            /**< AUTO_CFG_MAGIC */
    uint16_t version;          /**< AUTO_CFG_VERSION */
    uint16_t cfg_size;         /**< 写入时的 sizeof(automation_cfg_t)，用于尺寸自检 */
    automation_cfg_t cfg;      /**< 真正的配置 */
} auto_cfg_blob_t;

/* blob 头固定 8 字节（4+2+2），配置体从偏移 8 开始 ——
 * 这样 padding 不会跑到头里去，未来加字段时版本判断始终可靠。 */
_Static_assert(offsetof(auto_cfg_blob_t, cfg) == 8,
               "auto_cfg_blob_t header must stay exactly 8 bytes");
/* 配置体必须能用 uint16_t 记录尺寸（nvs_set_blob 也要求长度可控） */
_Static_assert(sizeof(automation_cfg_t) < 65535, "automation_cfg_t too large for the blob header");

/* ========================================================================= */
/*  默认配置                                                                 */
/* ========================================================================= */

/* CONFIG_APP_AUTO_ENABLE_DEFAULT 是 Kconfig 的 bool：开启时 sdkconfig.h 里定义为 1，
 * 关闭时【不定义】（不是定义为 0），所以这里必须用 #ifdef 而不是 #if。 */
#ifdef CONFIG_APP_AUTO_ENABLE_DEFAULT
#define AUTO_DEF_ENABLED    true
#else
#define AUTO_DEF_ENABLED    false
#endif

/** 默认配置（一次性宏，避免"静态初值"和"重置默认值"两处写重复） */
#define AUTO_CFG_DEFAULT_INIT {                    \
    .enabled            = AUTO_DEF_ENABLED,        \
    .light_on_lux       = (float)BSP_DEF_LIGHT_ON_LUX,   /* 50  */   \
    .light_off_lux      = (float)BSP_DEF_LIGHT_OFF_LUX,  /* 200 */  \
    .temp_fan_on_c      = (float)BSP_DEF_TEMP_FAN_ON_C,  /* 28 */   \
    .temp_fan_off_c     = (float)BSP_DEF_TEMP_FAN_OFF_C, /* 26 */   \
    .fan_auto_speed     = 70,                      \
    .rain_pct           = (float)BSP_DEF_RAIN_PCT, /* 30 */   \
    .auto_window_reopen = false,                   /* 安全起见，雨停不自动开窗 */ \
    .auto_light_enable  = true,                    \
    .auto_temp_enable   = true,                    \
    .auto_rain_enable   = true,                    \
}

/** 生效中的配置（受 s_lock 保护） */
static automation_cfg_t s_cfg = AUTO_CFG_DEFAULT_INIT;

/** 各设备最后一次被"手动操作"的时刻（ms，esp_timer 时基）；0 = 从未手动操作过 */
static int64_t s_last_manual_ms[DEV_COUNT];

static SemaphoreHandle_t s_lock  = NULL;
static bool              s_inited = false;

/* ========================================================================= */
/*  锁辅助                                                                   */
/* ========================================================================= */

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

/** 取配置快照（tick / json 用，避免长时间持锁） */
static void cfg_snapshot(automation_cfg_t *out)
{
    auto_lock();
    *out = s_cfg;
    auto_unlock();
}

/* ========================================================================= */
/*  配置校验                                                                 */
/* ========================================================================= */

/** @brief 校验一组配置是否自洽（从 NVS 读回来的数据必须过这一关） */
static bool cfg_validate(const automation_cfg_t *c)
{
    if (!isfinite(c->light_on_lux) || !isfinite(c->light_off_lux) ||
        !isfinite(c->temp_fan_on_c) || !isfinite(c->temp_fan_off_c) ||
        !isfinite(c->rain_pct)) {
        return false;
    }
    if (c->light_on_lux <= 0.0f) {
        return false;                                   /* 开灯阈值必须为正 */
    }
    if (c->light_off_lux <= c->light_on_lux) {
        return false;                                   /* 迟滞区间上下界反了 */
    }
    if ((c->temp_fan_on_c < -10.0f) || (c->temp_fan_on_c > 60.0f)) {
        return false;
    }
    if (c->temp_fan_off_c >= c->temp_fan_on_c) {
        return false;                                   /* 关风扇阈值必须低于开风扇阈值 */
    }
    if ((c->rain_pct < 0.0f) || (c->rain_pct > 100.0f)) {
        return false;
    }
    if (c->fan_auto_speed > 100) {
        return false;
    }
    return true;
}

/* ========================================================================= */
/*  初始化 / 持久化                                                          */
/* ========================================================================= */

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

    /* NVS 兜底初始化：main 里通常已经调用过 nvs_flash_init()，
     * 重复调用是安全（幂等）的，这里只是为了"单独用本模块"时也能跑起来。 */
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

    /* 读不到 / 版本不符 / 内容越界，automation_load 内部都会回落到默认值 */
    err = automation_load();
    s_inited = true;

    ESP_LOGI(TAG, "automation init done (%s)",
             (err == ESP_OK) ? "cfg loaded from NVS" : "using default cfg");

    /* JSON 最长约 220 字符（数值被改成很长的小数时会更长），这里给足余量 */
    char buf[320];
    if (automation_cfg_json(buf, sizeof(buf)) > 0) {
        ESP_LOGI(TAG, "cfg = %s", buf);
    }

    /* 初始化不失败：配置问题不应该让整机起不来 */
    return ESP_OK;
}

esp_err_t automation_load(void)
{
    automation_cfg_t loaded;
    cfg_set_defaults(&loaded);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(AUTO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* 第一次上电时命名空间还不存在，这是正常路径，不是错误 */
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
        err = nvs_commit(handle);       /* 不 commit 掉电可能丢 */
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

/* ========================================================================= */
/*  开关与配置访问                                                            */
/* ========================================================================= */

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

/**
 * @note 返回的是内部结构体地址，调用方可以直接改（例如批量改多个阈值），
 *       改完请调用 automation_save() 落盘。
 *       ⚠ 直接改指针里的字段【不经过本模块的锁】，与其他任务的并发写属于低概率事件；
 *         要严格安全就统一走 automation_set_threshold()。
 */
automation_cfg_t *automation_get_cfg(void)
{
    return &s_cfg;
}

/* ========================================================================= */
/*  按字符串键改阈值（MQTT config topic 用）                                  */
/* ========================================================================= */

esp_err_t automation_set_threshold(const char *key, float value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    bool      log_bool = false;
    bool      bool_val = false;

    /* NaN / Inf 一律视为非法值（JSON 解析出异常数值时挡住） */
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
        /* 低于此光照 → 开灯 / 合帘。
         * ★ on 必须严格低于 off，否则迟滞区间反转，tick 每 500ms 开关交替
         *   （实测：on=250 / off=200 时灯狂闪、窗帘舵机来回转）。 */
        if (value <= 0.0f || value >= s_cfg.light_off_lux) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.light_on_lux = value;
        }
    } else if (strcmp(key, "light_off_lux") == 0) {
        /* 高于此光照 → 关灯 / 开帘。
         * ★ light_on_lux 与 light_off_lux 之间就是【迟滞区间】：
         *   光照在这段区间里来回抖动时，灯和窗帘都保持原状态，不会反复开关。 */
        if (value <= s_cfg.light_on_lux) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.light_off_lux = value;
        }
    } else if (strcmp(key, "temp_fan_on_c") == 0) {
        /* ★ on 必须严格高于 off，否则迟滞区间反转 → 风扇每 500ms 开关一次
         *   （实测：on=20 / off=26 时无限振荡，MQTT 状态消息刷屏）。 */
        if ((value < -10.0f) || (value > 60.0f) || value <= s_cfg.temp_fan_off_c) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            s_cfg.temp_fan_on_c = value;
        }
    } else if (strcmp(key, "temp_fan_off_c") == 0) {
        /* 同理：off 必须低于 on，两者之差就是温度的迟滞区间 */
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
            s_cfg.fan_auto_speed = (uint8_t)(value + 0.5f);     /* 四舍五入到整数 % */
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
        err = ESP_ERR_INVALID_ARG;      /* 键不认识 */
    }

    auto_unlock();

    /* 日志放在解锁之后：ESPLOG 内部有锁，避免嵌套持有的时间被拉长 */
    if (err == ESP_OK) {
        if (log_bool) {
            ESP_LOGI(TAG, "cfg.%s = %s", key, bool_val ? "true" : "false");
        } else {
            ESP_LOGI(TAG, "cfg.%s = %.2f", key, (double)value);
        }
    } else {
        ESP_LOGW(TAG, "cfg.%s = %.2f rejected: %s", key, (double)value, esp_err_to_name(err));
    }

    /* 注意：这里【不自动 save】，由调用方决定何时落盘（例如一帧 config JSON 改多项后只存一次） */
    return err;
}

/* ========================================================================= */
/*  JSON 序列化                                                              */
/* ========================================================================= */

/** @brief 用一份配置快照组装 JSON（调用方自己保证缓冲区够大） */
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

/* ========================================================================= */
/*  手动保护期                                                               */
/* ========================================================================= */

void automation_notify_manual(device_id_t id)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;

    auto_lock();
    if (id == DEV_COUNT) {
        /* 约定：DEV_COUNT 表示"全部设备"（device_from_name("all") 也返回它），
         * 适合"全部关闭"这种一次动所有设备的操作。 */
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

/**
 * @brief 手动保护期检查
 * @return true = 允许自动规则动作；false = 该设备刚被手动操作过，本次跳过
 */
static bool guard_ok(device_id_t id, int64_t now_ms)
{
    if (((int)id < 0) || (id >= DEV_COUNT)) {
        return true;
    }

    int64_t last = s_last_manual_ms[id];
    if (last == 0) {
        return true;        /* 从未手动操作过（上电初期） */
    }
    return (now_ms - last) >= (int64_t)AUTO_MANUAL_GUARD_MS;
}

/* ========================================================================= */
/*  光照数据有效性                                                           */
/* ========================================================================= */

/**
 * @brief 判断光照数据是否可用
 *
 * sensor.h 没有单独的 light_valid 字段，这里按传感器类型约定：
 *   · BH1750 在线（light_is_bh1750）→ 永远有效；
 *   · 光敏电阻模式 → ADC 原始值 light_mv 或折算亮度 light_pct 至少一个非 0。
 * 目的：传感器没插（读数恒为 0）时，不要让"屋里很暗"的规则把灯一直点亮。
 */
static bool light_data_valid(const sensor_data_t *d)
{
    return d->light_is_bh1750 || (d->light_mv > 0) || (d->light_pct > 0.0f);
}

/* ========================================================================= */
/*  主循环 tick                                                              */
/* ========================================================================= */

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

    /* ================= 规则1：光照（需求 1） =================
     * 语义：暗 → 开客厅灯 + 合上窗帘；亮 → 关客厅灯 + 拉开窗帘。
     * 迟滞：只有 lux 低于 light_on_lux 才动作，且要高于 light_off_lux 才反向动作，
     *       中间区间保持不动。
     */
    if (cfg.auto_light_enable && light_data_valid(d)) {
        /* —— 客厅灯 —— */
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

        /* —— 窗帘（方向与灯相反：暗了要"拉上"= level 0；亮了要"拉开"= level 100）—— */
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

    /* ================= 规则2：温度（需求 2） =================
     * 迟滞：> temp_fan_on_c 开风扇，< temp_fan_off_c 才关风扇。
     */
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

    /* ================= 规则3：雨滴（需求 3） =================
     * 下雨判定用【当前配置里的阈值】现算，不用 d->rain_detected：
     * 那个标志位是传感器模块按自己的固定阈值算的，阈值被 MQTT 改过之后就对不上了。
     */
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
