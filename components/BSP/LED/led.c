/**
 * @file  led.c
 * @brief 灯带业务层实现（客厅 / 厨房 / 卧室 / 浴室）+ 板载 RGB 状态灯
 *
 * 【分层】
 *   本文件只负责"业务语义"（哪个房间、开关、亮度、颜色、状态灯语义），
 *   真正产生 WS2812 时序的是 ws2812.c（RMT 后端 / 位带后端）。
 *
 * 【刷新策略（重要）】
 *   apply_zone() 只改内存；为了让"单独调一个 set 函数也能立刻看到效果"，
 *   led_set_power / led_set_rgb / led_set_brightness 在 apply 之后【立即刷新该分区】
 *   （一条 30 颗约 1ms，可以接受）。
 *   成批修改请用 led_all_on() / led_all_off()，或者自己改完状态后调一次 led_flush()，
 *   那样只发一轮数据，不会有中间态闪烁。
 *
 * 【配件没到也能跑】
 *   led_init() 里每条灯带单独 try，失败只告警并把该分区标记为不可用，不影响其它分区。
 *
 * 所有引脚/灯珠数都取自 board_config.h，本文件不硬编码任何 GPIO 号。
 */
#include "led.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_assert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ws2812.h"
#include "driver/ledc.h"   /* 普通 LED 模式（BSP_LED_BACKEND_WS2812=0）用 LEDC PWM 调亮度 */

/** 编译期自检：后端开关只能是 0 或 1 */
#if (BSP_LED_BACKEND_WS2812 != 0) && (BSP_LED_BACKEND_WS2812 != 1)
#error "BSP_LED_BACKEND_WS2812 must be 0 or 1"
#endif

static const char *TAG = "LED";

/* 编译期自检：枚举数量必须和 board_config.h 对齐 */
ESP_STATIC_ASSERT(LED_ZONE_MAX == BSP_LED_ZONE_COUNT, "LED_ZONE_MAX must match BSP_LED_ZONE_COUNT");

/** 单个分区的静态状态 */
typedef struct {
    ws2812_strip_handle_t h;           /* 该分区的灯带句柄（未初始化/失败时为 NULL） */
    bool     power;                    /* 开 / 关 */
    uint8_t  brightness;               /* 0~100，默认 100 */
    uint8_t  r, g, b;                  /* 记忆的用户颜色，默认全白 */
    bool     inited;                   /* 该分区灯带是否创建成功 */
} led_zone_ctx_t;

/* 顺序必须和 led_zone_t 一致：LIVING / KITCHEN / BEDROOM / BATH */
static led_zone_ctx_t s_zone[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_KITCHEN] = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_BEDROOM] = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_BATH]    = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
};

/* 分区 → 硬件映射表：一一对应 board_config.h，不做任何硬编码 */
static const struct {
    gpio_num_t gpio;
    uint32_t   led_num;
} s_zone_hw[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = { BSP_WS2812_GPIO_LIVING,  BSP_WS2812_LED_NUM_LIVING  },
    [LED_ZONE_KITCHEN] = { BSP_WS2812_GPIO_KITCHEN, BSP_WS2812_LED_NUM_KITCHEN },
    [LED_ZONE_BEDROOM] = { BSP_WS2812_GPIO_BEDROOM, BSP_WS2812_LED_NUM_BEDROOM },
    [LED_ZONE_BATH]    = { BSP_WS2812_GPIO_BATH,    BSP_WS2812_LED_NUM_BATH    },
};

/* 分区英文名（顺序同上）：MQTT topic / 日志 / OLED 都用它 */
static const char *const s_zone_name[LED_ZONE_MAX] = { "living", "kitchen", "bedroom", "bath" };

static bool s_inited = false;

/* ------------------------------------------------------------------ */
/*  内部工具                                                           */
/* ------------------------------------------------------------------ */

static bool led_zone_valid(led_zone_t zone)
{
    return ((int)zone >= 0) && (zone < LED_ZONE_MAX);
}

#if BSP_LED_BACKEND_WS2812
/* ============================ 后端 A：WS2812 灯带 ============================ */

/**
 * @brief 按当前状态算颜色并写进灯带内存（【不】发送）
 * @note  断电 → 全黑；通电 → r/g/b 各乘 brightness/100
 */
static esp_err_t apply_zone(led_zone_t zone)
{
    if (!s_zone[zone].inited || s_zone[zone].h == NULL) {
        return ESP_OK;   /* 这条灯带没接上：只保留软件状态，不算错误 */
    }

    uint8_t r = 0, g = 0, b = 0;

    if (s_zone[zone].power) {
        const uint32_t br = (uint32_t)s_zone[zone].brightness;   /* 0~100 */
        r = (uint8_t)((uint32_t)s_zone[zone].r * br / 100U);
        g = (uint8_t)((uint32_t)s_zone[zone].g * br / 100U);
        b = (uint8_t)((uint32_t)s_zone[zone].b * br / 100U);
    }

    esp_err_t err = ws2812_set_all(s_zone[zone].h, r, g, b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ws2812_set_all failed: %s", s_zone_name[zone], esp_err_to_name(err));
    }
    return err;
}

/** 把该分区的内存数据真正发到灯带 */
static esp_err_t zone_refresh(led_zone_t zone)
{
    if (!s_zone[zone].inited || s_zone[zone].h == NULL) {
        return ESP_OK;
    }

    esp_err_t err = ws2812_refresh(s_zone[zone].h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ws2812_refresh failed: %s", s_zone_name[zone], esp_err_to_name(err));
    }
    return err;
}

#else
/* ================= 后端 B：普通单色 LED 模块（LEDC PWM 调亮度） =================
 * 每路一个 GPIO，直接推 LED 模块的 S 脚。
 * ★ 没有颜色：led_set_rgb() 只更新软件状态，硬件上不体现。
 *   亮度用 LEDC PWM 实现（0~100% 映射到 0~255 占空比）。 */

static const ledc_channel_t s_plain_ch[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = LEDC_CHANNEL_4,
    [LED_ZONE_KITCHEN] = LEDC_CHANNEL_5,
    [LED_ZONE_BEDROOM] = LEDC_CHANNEL_6,
    [LED_ZONE_BATH]    = LEDC_CHANNEL_7,
};

static esp_err_t apply_zone(led_zone_t zone)
{
    if (!s_zone[zone].inited) {
        return ESP_OK;   /* 这路 LED 没接上：只保留软件状态 */
    }

    uint32_t duty = 0;
    if (s_zone[zone].power) {
        duty = (uint32_t)s_zone[zone].brightness * BSP_LED_PLAIN_DUTY_MAX / 100U;
    }

#if BSP_LED_PLAIN_ACTIVE_LOW
    duty = BSP_LED_PLAIN_DUTY_MAX - duty;
#endif

    esp_err_t err = ledc_set_duty(BSP_LED_PLAIN_MODE, s_plain_ch[zone], duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BSP_LED_PLAIN_MODE, s_plain_ch[zone]);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ledc duty set failed: %s", s_zone_name[zone], esp_err_to_name(err));
    }
    return err;
}

/** 普通 LED 模式下 apply_zone 已经直接作用到硬件了，无需再刷新 */
static esp_err_t zone_refresh(led_zone_t zone)
{
    (void)zone;
    return ESP_OK;
}

#endif /* BSP_LED_BACKEND_WS2812 */

/** 批量开关：改完全部分区状态后只刷一次，避免中间态闪烁 */
static esp_err_t led_apply_all(bool on)
{
    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < (int)LED_ZONE_MAX; i++) {
        s_zone[i].power = on;
        const esp_err_t err = apply_zone((led_zone_t)i);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    const esp_err_t flush_err = led_flush();
    return (first_err != ESP_OK) ? first_err : flush_err;
}

/* ------------------------------------------------------------------ */
/*  灯带控制                                                           */
/* ------------------------------------------------------------------ */

esp_err_t led_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    int ok_cnt = 0;

#if BSP_LED_BACKEND_WS2812
    ESP_LOGI(TAG, "backend: WS2812 addressable strip (colour + brightness)");

    for (int i = 0; i < (int)LED_ZONE_MAX; i++) {
        s_zone[i].h      = NULL;
        s_zone[i].inited = false;

        esp_err_t err = ws2812_new_strip(s_zone_hw[i].gpio, s_zone_hw[i].led_num, &s_zone[i].h);
        if (err != ESP_OK || s_zone[i].h == NULL) {
            if (err == ESP_OK) {
                err = ESP_ERR_INVALID_STATE;   /* 理论上不会发生，防呆 */
            }
            /* 单条失败只告警：配件没到也要能跑，不影响其它分区 */
            ESP_LOGW(TAG, "  %-7s gpio=%-2d leds=%u FAILED: %s -> zone disabled",
                     s_zone_name[i], (int)s_zone_hw[i].gpio, (unsigned)s_zone_hw[i].led_num,
                     esp_err_to_name(err));
            s_zone[i].h = NULL;
            continue;
        }

        /* 默认状态：关灯、亮度 100%、全白（颜色被记住，下次开灯沿用） */
        s_zone[i].inited     = true;
        s_zone[i].power      = false;
        s_zone[i].brightness = 100;
        s_zone[i].r          = 255;
        s_zone[i].g          = 255;
        s_zone[i].b          = 255;
        ok_cnt++;

        ESP_LOGI(TAG, "  %-7s gpio=%-2d leds=%u OK",
                 s_zone_name[i], (int)s_zone_hw[i].gpio, (unsigned)s_zone_hw[i].led_num);
    }

#else
    ESP_LOGI(TAG, "backend: plain single-colour LED module (LEDC PWM, NO colour)");

    /* 4 路 LED 共用一个 LEDC 定时器（舵机用 timer0、风扇用 timer1，这里用 timer2） */
    {
        ledc_timer_config_t tcfg = {
            .speed_mode      = BSP_LED_PLAIN_MODE,
            .duty_resolution = BSP_LED_PLAIN_RES,
            .timer_num       = BSP_LED_PLAIN_TIMER,
            .freq_hz         = BSP_LED_PLAIN_FREQ_HZ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        esp_err_t terr = ledc_timer_config(&tcfg);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(terr));
            return terr;
        }
    }

    for (int i = 0; i < (int)LED_ZONE_MAX; i++) {
        s_zone[i].h      = NULL;
        s_zone[i].inited = false;

        ledc_channel_config_t ccfg = {
            .gpio_num   = s_zone_hw[i].gpio,
            .speed_mode = BSP_LED_PLAIN_MODE,
            .channel    = s_plain_ch[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = BSP_LED_PLAIN_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };

        esp_err_t err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) {
            /* 单路失败只告警：配件没到也要能跑，不影响其它路 */
            ESP_LOGW(TAG, "  %-7s gpio=%-2d FAILED: %s -> zone disabled",
                     s_zone_name[i], (int)s_zone_hw[i].gpio, esp_err_to_name(err));
            continue;
        }

        /* 默认状态：关灯、亮度 100%（颜色字段保留，普通 LED 用不上） */
        s_zone[i].inited     = true;
        s_zone[i].power      = false;
        s_zone[i].brightness = 100;
        s_zone[i].r          = 255;
        s_zone[i].g          = 255;
        s_zone[i].b          = 255;
        ok_cnt++;

        /* 立刻按"关灯"状态写一次占空比（自动处理低电平点亮的情况） */
        (void)apply_zone((led_zone_t)i);

        ESP_LOGI(TAG, "  %-7s gpio=%-2d OK%s",
                 s_zone_name[i], (int)s_zone_hw[i].gpio,
#if BSP_LED_PLAIN_ACTIVE_LOW
                 " (active-LOW)"
#else
                 " (active-HIGH)"
#endif
                 );
    }
#endif /* BSP_LED_BACKEND_WS2812 */

    /* 上电灯带里可能是残余颜色，全部刷黑 */
    (void)led_flush();

    s_inited = true;

#if BSP_STATUS_LED_ENABLE
    /* 板载状态灯：进入"启动中"（蓝色慢闪） */
    (void)led_status_set(LED_STATUS_BOOT);
#else
    ESP_LOGI(TAG, "onboard status led disabled (BSP_STATUS_LED_ENABLE = 0)");
#endif

    if (ok_cnt == 0) {
        /* 一条灯带都没有：不是致命错误（可能配件还没到），但要让调用方知道 */
        ESP_LOGW(TAG, "no led strip available (0/%d), check data line and 5V supply",
                 (int)LED_ZONE_MAX);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "init ok: %d/%d zone(s) ready", ok_cnt, (int)LED_ZONE_MAX);
    return ESP_OK;
}

esp_err_t led_set_power(led_zone_t zone, bool on)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_zone[zone].power = on;

    esp_err_t err = apply_zone(zone);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s -> %s (br=%u%% rgb=%u,%u,%u)", s_zone_name[zone], on ? "ON" : "OFF",
             (unsigned)s_zone[zone].brightness,
             (unsigned)s_zone[zone].r, (unsigned)s_zone[zone].g, (unsigned)s_zone[zone].b);

    /* 折中方案：立刻刷新本分区（约 1ms），保证单独调一次就生效。
     * 成批修改请最后统一调 led_flush()。 */
    return zone_refresh(zone);
}

bool led_get_power(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone[zone].power : false;
}

esp_err_t led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 记住颜色，关灯再开灯时沿用 */
    s_zone[zone].r = r;
    s_zone[zone].g = g;
    s_zone[zone].b = b;

    esp_err_t err = apply_zone(zone);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s color -> %u,%u,%u (power=%d br=%u%%)", s_zone_name[zone],
             (unsigned)r, (unsigned)g, (unsigned)b, (int)s_zone[zone].power,
             (unsigned)s_zone[zone].brightness);

    return zone_refresh(zone);
}

esp_err_t led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!led_zone_valid(zone) || r == NULL || g == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *r = s_zone[zone].r;
    *g = s_zone[zone].g;
    *b = s_zone[zone].b;
    return ESP_OK;
}

esp_err_t led_set_brightness(led_zone_t zone, uint8_t percent)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    s_zone[zone].brightness = percent;

    esp_err_t err = apply_zone(zone);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s brightness -> %u%%", s_zone_name[zone], (unsigned)percent);
    return zone_refresh(zone);
}

uint8_t led_get_brightness(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone[zone].brightness : 0;
}

esp_err_t led_toggle(led_zone_t zone)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_set_power(zone, !s_zone[zone].power);
}

esp_err_t led_all_off(void)
{
    ESP_LOGI(TAG, "all zones -> OFF");
    return led_apply_all(false);
}

esp_err_t led_all_on(void)
{
    ESP_LOGI(TAG, "all zones -> ON");
    return led_apply_all(true);
}

esp_err_t led_flush(void)
{
    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < (int)LED_ZONE_MAX; i++) {
        const esp_err_t err = zone_refresh((led_zone_t)i);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

const char *led_zone_name(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone_name[zone] : "unknown";
}

led_zone_t led_zone_from_name(const char *name)
{
    if (name == NULL) {
        return LED_ZONE_MAX;
    }

    for (int i = 0; i < (int)LED_ZONE_MAX; i++) {
        if (strcmp(name, s_zone_name[i]) == 0) {
            return (led_zone_t)i;
        }
    }
    return LED_ZONE_MAX;
}

/* ------------------------------------------------------------------ */
/*  板载 RGB 状态灯（WS2812，位带驱动，只有 1 颗）                      */
/* ------------------------------------------------------------------ */
#if BSP_STATUS_LED_ENABLE

/** 状态 → 颜色 / 闪烁节拍 映射表（下标 = led_status_t） */
typedef struct {
    uint8_t  r, g, b;
    bool     blink;
    uint32_t period_ms;
} led_status_cfg_t;

static const led_status_cfg_t s_status_cfg[] = {
    [LED_STATUS_BOOT]            = { .r = 0,   .g = 0,   .b = 255, .blink = true,  .period_ms = 500 },
    [LED_STATUS_WIFI_CONNECTING] = { .r = 255, .g = 180, .b = 0,   .blink = true,  .period_ms = 500 },
    [LED_STATUS_WIFI_OK]         = { .r = 0,   .g = 255, .b = 0,   .blink = false, .period_ms = 0   },
    [LED_STATUS_MQTT_OK]         = { .r = 0,   .g = 255, .b = 255, .blink = false, .period_ms = 0   },
    [LED_STATUS_ERROR]           = { .r = 255, .g = 0,   .b = 0,   .blink = true,  .period_ms = 150 },
    [LED_STATUS_AP_MODE]         = { .r = 180, .g = 0,   .b = 255, .blink = true,  .period_ms = 500 },
};

ESP_STATIC_ASSERT((sizeof(s_status_cfg) / sizeof(s_status_cfg[0])) == (size_t)(LED_STATUS_AP_MODE + 1),
                  "s_status_cfg must cover every led_status_t");

static const char *const s_status_name[] = {
    [LED_STATUS_BOOT]            = "boot",
    [LED_STATUS_WIFI_CONNECTING] = "wifi_connecting",
    [LED_STATUS_WIFI_OK]         = "wifi_ok",
    [LED_STATUS_MQTT_OK]         = "mqtt_ok",
    [LED_STATUS_ERROR]           = "error",
    [LED_STATUS_AP_MODE]         = "ap_mode",
};

static esp_timer_handle_t s_status_timer = NULL;   /* 只创建一次 */
static led_status_t       s_status_cur   = LED_STATUS_BOOT;
static bool               s_status_on    = false;  /* 当前是亮还是灭 */
static uint8_t            s_status_r = 0, s_status_g = 0, s_status_b = 255;

/**
 * @brief 闪烁回调
 * @note  运行在 esp_timer 任务上下文；位带写 1 颗 WS2812 关中断约 30us，可以接受。
 *        本函数只翻转亮灭，绝不调用会阻塞的接口。
 */
static void status_timer_cb(void *arg)
{
    (void)arg;

    s_status_on = !s_status_on;

    if (s_status_on) {
        (void)led_status_rgb(s_status_r, s_status_g, s_status_b);
    } else {
        (void)led_status_rgb(0, 0, 0);
    }
}

static esp_err_t status_timer_ensure(void)
{
    if (s_status_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback             = status_timer_cb,
        .arg                  = NULL,
        .dispatch_method      = ESP_TIMER_TASK,
        .name                 = "led_status",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&args, &s_status_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status timer create failed: %s", esp_err_to_name(err));
    }
    return err;
}

#endif /* BSP_STATUS_LED_ENABLE */

esp_err_t led_status_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if BSP_STATUS_LED_ENABLE
    /* WS2812 的数据顺序是 GRB —— 缓冲里 G 必须排第一 */
    const uint8_t grb[3] = { g, r, b };
    return ws2812_bitbang_write(BSP_STATUS_LED_GPIO, grb, 1);
#else
    (void)r;
    (void)g;
    (void)b;
    return ESP_OK;   /* 未启用：空实现 */
#endif
}

esp_err_t led_status_set(led_status_t st)
{
#if !BSP_STATUS_LED_ENABLE
    (void)st;
    return ESP_OK;   /* 未启用：空实现 */
#else
    if ((int)st < 0 || st > LED_STATUS_AP_MODE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = status_timer_ensure();
    if (err != ESP_OK) {
        return err;
    }

    /* 切状态先停掉旧节拍；之前没在跑会返回 ESP_ERR_INVALID_STATE，忽略 */
    (void)esp_timer_stop(s_status_timer);

    const led_status_cfg_t *cfg = &s_status_cfg[st];

    s_status_cur = st;
    s_status_r   = cfg->r;
    s_status_g   = cfg->g;
    s_status_b   = cfg->b;
    s_status_on  = true;

    /* 立刻点亮，不要等到第一个周期 */
    err = led_status_rgb(cfg->r, cfg->g, cfg->b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status %s: write failed: %s", s_status_name[s_status_cur], esp_err_to_name(err));
        return err;
    }

    if (!cfg->blink) {
        ESP_LOGI(TAG, "status led -> %s (steady)", s_status_name[s_status_cur]);
        return ESP_OK;   /* 常亮：定时器保持停止，s_status_on = true */
    }

    err = esp_timer_start_periodic(s_status_timer, (uint64_t)cfg->period_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status timer start(%ums) failed: %s",
                 (unsigned)cfg->period_ms, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "status led -> %s (blink %ums)", s_status_name[s_status_cur], (unsigned)cfg->period_ms);
    return ESP_OK;
#endif /* BSP_STATUS_LED_ENABLE */
}
