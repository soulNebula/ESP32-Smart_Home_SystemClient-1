#include "led.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_assert.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ws2812.h"
// 普通 LED 用调光
#include "driver/ledc.h"

// 检查后端开关取值
#if (BSP_LED_BACKEND_WS2812 != 0) && (BSP_LED_BACKEND_WS2812 != 1)
#error "BSP_LED_BACKEND_WS2812 must be 0 or 1"
#endif

static const char *TAG = "LED";

// 核对分区数量
ESP_STATIC_ASSERT(LED_ZONE_MAX == BSP_LED_ZONE_COUNT, "LED_ZONE_MAX must match BSP_LED_ZONE_COUNT");

// 存一路灯的当前状态
typedef struct {
    // 这路灯带的句柄
    ws2812_strip_handle_t h;
    // 开着还是关着
    bool     power;
    // 亮度 0 到 100
    uint8_t  brightness;
    // 记住的颜色
    uint8_t  r, g, b;
    // 这路是否接上
    bool     inited;
} led_zone_ctx_t;

// 四个房间的初始状态
static led_zone_ctx_t s_zone[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_KITCHEN] = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_BEDROOM] = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
    [LED_ZONE_BATH]    = { .h = NULL, .power = false, .brightness = 100, .r = 255, .g = 255, .b = 255, .inited = false },
};

// 对照板级表找引脚
static const struct {
    gpio_num_t gpio;
    uint32_t   led_num;
} s_zone_hw[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = { BSP_WS2812_GPIO_LIVING,  BSP_WS2812_LED_NUM_LIVING  },
    [LED_ZONE_KITCHEN] = { BSP_WS2812_GPIO_KITCHEN, BSP_WS2812_LED_NUM_KITCHEN },
    [LED_ZONE_BEDROOM] = { BSP_WS2812_GPIO_BEDROOM, BSP_WS2812_LED_NUM_BEDROOM },
    [LED_ZONE_BATH]    = { BSP_WS2812_GPIO_BATH,    BSP_WS2812_LED_NUM_BATH    },
};

// 给房间起英文名
static const char *const s_zone_name[LED_ZONE_MAX] = { "living", "kitchen", "bedroom", "bath" };

static bool s_inited = false;

// 判断分区号合不合法
static bool led_zone_valid(led_zone_t zone)
{
    return ((int)zone >= 0) && (zone < LED_ZONE_MAX);
}

#if BSP_LED_BACKEND_WS2812

// 算好颜色写进内存
static esp_err_t apply_zone(led_zone_t zone)
{
    if (!s_zone[zone].inited || s_zone[zone].h == NULL) {
        // 没接就当没这路
        return ESP_OK;
    }

    uint8_t r = 0, g = 0, b = 0;

    if (s_zone[zone].power) {
        // 亮度 0 到 100
        const uint32_t br = (uint32_t)s_zone[zone].brightness;
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

// 把数据发给灯带
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
// 换成普通单色灯模块

static const ledc_channel_t s_plain_ch[LED_ZONE_MAX] = {
    [LED_ZONE_LIVING]  = LEDC_CHANNEL_4,
    [LED_ZONE_KITCHEN] = LEDC_CHANNEL_5,
    [LED_ZONE_BEDROOM] = LEDC_CHANNEL_6,
    [LED_ZONE_BATH]    = LEDC_CHANNEL_7,
};

// 按亮度写占空比
static esp_err_t apply_zone(led_zone_t zone)
{
    if (!s_zone[zone].inited) {
        // 没接就当没这路
        return ESP_OK;
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

// 这种灯已直接生效
static esp_err_t zone_refresh(led_zone_t zone)
{
    (void)zone;
    return ESP_OK;
}

// 灯带后端收尾
#endif

// 整批开关只刷新一次
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

// 把硬件准备好
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
                // 正常不该走到这里
                err = ESP_ERR_INVALID_STATE;
            }
            // 单路坏了不影响别的
            ESP_LOGW(TAG, "  %-7s gpio=%-2d leds=%u FAILED: %s -> zone disabled",
                     s_zone_name[i], (int)s_zone_hw[i].gpio, (unsigned)s_zone_hw[i].led_num,
                     esp_err_to_name(err));
            s_zone[i].h = NULL;
            continue;
        }

        // 开机默认关灯全白
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

    // 四路灯共用一个定时器
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
            // 单路坏了不影响别的
            ESP_LOGW(TAG, "  %-7s gpio=%-2d FAILED: %s -> zone disabled",
                     s_zone_name[i], (int)s_zone_hw[i].gpio, esp_err_to_name(err));
            continue;
        }

        // 开机默认关灯全白
        s_zone[i].inited     = true;
        s_zone[i].power      = false;
        s_zone[i].brightness = 100;
        s_zone[i].r          = 255;
        s_zone[i].g          = 255;
        s_zone[i].b          = 255;
        ok_cnt++;

        // 先按关灯写一次
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
// 灯带后端收尾
#endif

    // 上电先全刷黑
    (void)led_flush();

    s_inited = true;

#if BSP_STATUS_LED_ENABLE
    // 状态灯先进启动中
    (void)led_status_set(LED_STATUS_BOOT);
#else
    ESP_LOGI(TAG, "onboard status led disabled (BSP_STATUS_LED_ENABLE = 0)");
#endif

    if (ok_cnt == 0) {
        // 一路都没有要报上
        ESP_LOGW(TAG, "no led strip available (0/%d), check data line and 5V supply",
                 (int)LED_ZONE_MAX);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "init ok: %d/%d zone(s) ready", ok_cnt, (int)LED_ZONE_MAX);
    return ESP_OK;
}

// 开或关某路灯
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

    // 单次调用马上见效
    return zone_refresh(zone);
}

// 查某路灯开着吗
bool led_get_power(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone[zone].power : false;
}

// 调某路灯的颜色
esp_err_t led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }

    // 颜色留着下次用
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

// 读某路灯的颜色
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

// 调某路灯的亮度
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

// 读某路灯的亮度
uint8_t led_get_brightness(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone[zone].brightness : 0;
}

// 把某路灯反过来
esp_err_t led_toggle(led_zone_t zone)
{
    if (!led_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_set_power(zone, !s_zone[zone].power);
}

// 四路灯全关
esp_err_t led_all_off(void)
{
    ESP_LOGI(TAG, "all zones -> OFF");
    return led_apply_all(false);
}

// 四路灯全开
esp_err_t led_all_on(void)
{
    ESP_LOGI(TAG, "all zones -> ON");
    return led_apply_all(true);
}

// 把数据统一发出去
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

// 把分区号换成名字
const char *led_zone_name(led_zone_t zone)
{
    return led_zone_valid(zone) ? s_zone_name[zone] : "unknown";
}

// 由名字反查分区号
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

// 板上一颗状态灯
#if BSP_STATUS_LED_ENABLE

// 状态对应颜色和闪法
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

// 状态对应的名字
static const char *const s_status_name[] = {
    [LED_STATUS_BOOT]            = "boot",
    [LED_STATUS_WIFI_CONNECTING] = "wifi_connecting",
    [LED_STATUS_WIFI_OK]         = "wifi_ok",
    [LED_STATUS_MQTT_OK]         = "mqtt_ok",
    [LED_STATUS_ERROR]           = "error",
    [LED_STATUS_AP_MODE]         = "ap_mode",
};

// 只建一次
static esp_timer_handle_t s_status_timer = NULL;
static led_status_t       s_status_cur   = LED_STATUS_BOOT;
// 现在是亮是灭
static bool               s_status_on    = false;
static uint8_t            s_status_r = 0, s_status_g = 0, s_status_b = 255;

// 定时翻亮灭
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

// 没有就建闪灯定时器
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

// 状态灯开关收尾
#endif

// 直接写状态灯颜色
esp_err_t led_status_rgb(uint8_t r, uint8_t g, uint8_t b)
{
#if BSP_STATUS_LED_ENABLE
    // 灯珠要绿在前
    const uint8_t grb[3] = { g, r, b };
    return ws2812_bitbang_write(BSP_STATUS_LED_GPIO, grb, 1);
#else
    (void)r;
    (void)g;
    (void)b;
    // 没启用就空着
    return ESP_OK;
#endif
}

// 切状态灯的花样
esp_err_t led_status_set(led_status_t st)
{
#if !BSP_STATUS_LED_ENABLE
    (void)st;
    // 没启用就空着
    return ESP_OK;
#else
    if ((int)st < 0 || st > LED_STATUS_AP_MODE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = status_timer_ensure();
    if (err != ESP_OK) {
        return err;
    }

    // 换花样先停旧节拍
    (void)esp_timer_stop(s_status_timer);

    const led_status_cfg_t *cfg = &s_status_cfg[st];

    s_status_cur = st;
    s_status_r   = cfg->r;
    s_status_g   = cfg->g;
    s_status_b   = cfg->b;
    s_status_on  = true;

    // 马上亮不等下一轮
    err = led_status_rgb(cfg->r, cfg->g, cfg->b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status %s: write failed: %s", s_status_name[s_status_cur], esp_err_to_name(err));
        return err;
    }

    if (!cfg->blink) {
        ESP_LOGI(TAG, "status led -> %s (steady)", s_status_name[s_status_cur]);
        // 常亮就不用定时器
        return ESP_OK;
    }

    // 起循环闪灯
    err = esp_timer_start_periodic(s_status_timer, (uint64_t)cfg->period_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status timer start(%ums) failed: %s",
                 (unsigned)cfg->period_ms, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "status led -> %s (blink %ums)", s_status_name[s_status_cur], (unsigned)cfg->period_ms);
    return ESP_OK;
// 状态灯开关收尾
#endif
}
