#include "led.h"

#include "app_config.h"
#include "pwm_compat.h"

// ======================================================================
// 四路房间灯 + 板载状态灯
//
// 对应 ESP-IDF 工程的 components/BSP/LED/led.c（普通单色 LED 那个后端）。
// 这版 LED 模块是 3 脚白光模块（VCC/GND/S），只能调亮度，
// 颜色照样记在状态里，上报给手机时格式不变。
// ======================================================================

// 每路灯的状态
typedef struct {
    // 信号脚
    uint8_t pin;
    // LEDC 通道，2.x 的核心包按通道号写
    uint8_t channel;
    // 开关
    bool    power;
    // 亮度百分比
    uint8_t level;
    // 记着的颜色，硬件是白光，只是状态
    uint8_t r, g, b;
} led_zone_ctx_t;

// 顺序和 led_zone_t 一样，别乱
static led_zone_ctx_t s_zone[LED_ZONE_MAX] = {
    { BSP_LED_GPIO_LIVING,  BSP_LED_CH_LIVING,  false, 100, 255, 255, 255 },
    { BSP_LED_GPIO_KITCHEN, BSP_LED_CH_KITCHEN, false, 100, 255, 255, 255 },
    { BSP_LED_GPIO_BEDROOM, BSP_LED_CH_BEDROOM, false, 100, 255, 255, 255 },
    { BSP_LED_GPIO_BATH,    BSP_LED_CH_BATH,    false, 100, 255, 255, 255 },
};

// 四个分区的名字，日志和上报都用它
static const char *const s_zone_name[LED_ZONE_MAX] = {
    "living", "kitchen", "bedroom", "bath",
};

static bool s_inited = false;

// ---- 板载状态灯 ----
// 两个开关都开着才去驱动那颗灯
#if BSP_STATUS_LED_ENABLE && APP_STATUS_LED_ENABLE
#define LED_STATUS_LED_ON 1
#else
#define LED_STATUS_LED_ON 0
#endif

static led_status_t s_status    = LED_STATUS_BOOT;
// 上次翻灯的时刻
static uint32_t     s_status_ms = 0;
// 现在灯是亮着还是灭着
static bool         s_status_on = false;
// 上次真写下去的颜色，一样就不重复写
static uint8_t      s_last_r = 0, s_last_g = 0, s_last_b = 0;
static bool         s_last_valid = false;

// 亮度百分比换成占空比
static uint32_t led_duty_from_level(uint8_t level) {
    const uint32_t max = (1u << BSP_LED_RES_BITS) - 1u;
    uint32_t duty = ((uint32_t)level * max) / 100u;

#if BSP_LED_ACTIVE_LOW
    // 低电平点亮就反过来，关灯时占空比拉满
    duty = max - duty;
#endif

    return duty;
}

// 分区号合不合法
static bool led_zone_valid(led_zone_t zone) {
    return ((int)zone >= 0) && (zone < LED_ZONE_MAX);
}

// 把一路真正写到硬件
static void led_write_hw(int idx) {
    // 关着就给灭灯的占空比：低电平点亮时 led_duty_from_level(0) 自己会翻成满占空比，
    // 所以这里不用再分情况；开着才按亮度来
    const uint32_t duty = s_zone[idx].power ? led_duty_from_level(s_zone[idx].level)
                                            : led_duty_from_level(0);

    pwm_write(s_zone[idx].channel, s_zone[idx].pin, duty);
}

// 状态灯的花样换成颜色，灭的时候给全黑
static void led_status_color(led_status_t st, bool on, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!on) {
        // 灭
        *r = 0;
        *g = 0;
        *b = 0;
        return;
    }

    switch (st) {
    // 启动蓝灯慢闪
    case LED_STATUS_BOOT:
        *r = 0;  *g = 0;  *b = 90;
        break;
    // 连网黄灯慢闪
    case LED_STATUS_WIFI_CONNECTING:
        *r = 90; *g = 60; *b = 0;
        break;
    // 网通绿灯常亮
    case LED_STATUS_WIFI_OK:
        *r = 0;  *g = 90; *b = 0;
        break;
    // 上云青灯常亮
    case LED_STATUS_MQTT_OK:
        *r = 0;  *g = 70; *b = 70;
        break;
    // 出错红灯快闪
    case LED_STATUS_ERROR:
    default:
        *r = 90; *g = 0;  *b = 0;
        break;
    }
}

// 状态灯多久翻一次
static uint32_t led_status_period(led_status_t st) {
    switch (st) {
    // 快闪
    case LED_STATUS_ERROR:
        return 150;
    // 常亮，周期给 0 表示不翻
    case LED_STATUS_WIFI_OK:
    case LED_STATUS_MQTT_OK:
        return 0;
    // 慢闪
    default:
        return 500;
    }
}

// 把硬件准备好
bool led_init(void) {
    if (s_inited) {
        return true;
    }

    for (int i = 0; i < LED_ZONE_MAX; i++) {
        if (!pwm_attach(s_zone[i].channel, s_zone[i].pin,
                        BSP_LED_FREQ_HZ, BSP_LED_RES_BITS)) {
            Serial.printf("LED: %s gpio=%d PWM 挂不上\n", s_zone_name[i], s_zone[i].pin);
            return false;
        }

        // 上电先灭着
        s_zone[i].power = false;
        led_write_hw(i);

        Serial.printf("LED:   %-7s gpio=%-2d OK (%s)\n", s_zone_name[i], s_zone[i].pin,
                      BSP_LED_ACTIVE_LOW ? "active-LOW" : "active-HIGH");
    }

    s_inited = true;
    Serial.printf("LED: init ok: %d/4 zone(s) ready\n", LED_ZONE_MAX);
    return true;
}

bool led_set_power(led_zone_t zone, bool on) {
    if (!s_inited || !led_zone_valid(zone)) {
        return false;
    }

    s_zone[zone].power = on;
    led_write_hw((int)zone);
    return true;
}

bool led_get_power(led_zone_t zone) {
    if (!led_zone_valid(zone)) {
        return false;
    }
    return s_zone[zone].power;
}

// 设颜色并记住它，硬件是白光，只记状态
bool led_set_rgb(led_zone_t zone, uint8_t r, uint8_t g, uint8_t b) {
    if (!led_zone_valid(zone)) {
        return false;
    }

    s_zone[zone].r = r;
    s_zone[zone].g = g;
    s_zone[zone].b = b;
    return true;
}

bool led_get_rgb(led_zone_t zone, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!led_zone_valid(zone) || (r == NULL) || (g == NULL) || (b == NULL)) {
        return false;
    }

    *r = s_zone[zone].r;
    *g = s_zone[zone].g;
    *b = s_zone[zone].b;
    return true;
}

// 设亮度百分比
bool led_set_brightness(led_zone_t zone, uint8_t percent) {
    if (!s_inited || !led_zone_valid(zone)) {
        return false;
    }

    if (percent > 100) {
        percent = 100;
    }

    s_zone[zone].level = percent;
    led_write_hw((int)zone);
    return true;
}

uint8_t led_get_brightness(led_zone_t zone) {
    if (!led_zone_valid(zone)) {
        return 0;
    }
    return s_zone[zone].level;
}

bool led_toggle(led_zone_t zone) {
    if (!led_zone_valid(zone)) {
        return false;
    }
    return led_set_power(zone, !s_zone[zone].power);
}

bool led_all_off(void) {
    for (int i = 0; i < LED_ZONE_MAX; i++) {
        led_set_power((led_zone_t)i, false);
    }
    return led_flush();
}

bool led_all_on(void) {
    for (int i = 0; i < LED_ZONE_MAX; i++) {
        led_set_power((led_zone_t)i, true);
    }
    return led_flush();
}

// 改完一批再统一刷，单色灯其实就是再写一遍
bool led_flush(void) {
    for (int i = 0; i < LED_ZONE_MAX; i++) {
        led_write_hw(i);
    }
    return true;
}

// 分区号换英文名
const char *led_zone_name(led_zone_t zone) {
    return led_zone_valid(zone) ? s_zone_name[zone] : "unknown";
}

// 板载状态灯：换花样，下一拍 led_poll 里生效
bool led_status_set(led_status_t st) {
    s_status    = st;
    s_status_ms = millis();
    s_status_on = true;
    return true;
}

// 主循环喊这个，闪烁的花样靠它走
void led_poll(void) {
#if LED_STATUS_LED_ON
    const uint32_t period = led_status_period(s_status);

    // 常亮的两种不用翻
    if (period == 0) {
        s_status_on = true;
    } else if ((uint32_t)(millis() - s_status_ms) >= period) {
        s_status_ms = millis();
        s_status_on = !s_status_on;
    }

    uint8_t r, g, b;
    led_status_color(s_status, s_status_on, &r, &g, &b);

    // 颜色没变就别写：写 WS2812 要关中断几十微秒，写太勤会拖累蓝牙和网络
    if (s_last_valid && (r == s_last_r) && (g == s_last_g) && (b == s_last_b)) {
        return;
    }
    s_last_r = r;
    s_last_g = g;
    s_last_b = b;
    s_last_valid = true;

    status_led_write(BSP_STATUS_LED_GPIO, r, g, b);
#endif
}
