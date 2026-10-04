/*
 * 模块：
 *   设备总状态表。全屋每台设备的开关、亮度、颜色只记在这一份里，
 *   是本项目唯一的真相来源：语音、按键、手机、自动联动都只改它，
 *   界面刷新、上报手机、联动判断也只读它，不会各处各存一套对不上。
 *   被 automation.c、mqtt_app.c、ble_app.c、main.c 调用；
 *   自己向下调 led、fan、servo 三个硬件模块去动真设备。
 *
 *   这里的每个动作都分三步走：先改状态、再动硬件、最后通知上层，
 *   并且顺手记下这次是谁改的（人还是自动），自动联动靠它决定让不让手。
 *   状态数组用一把锁护着，几个人同时改也不会写乱；通知上层一律放到
 *   放锁之后，免得在锁里转回来又调自己。设备到硬件的对照是一路一路
 *   写死的，不靠两边的序号碰巧一致。
 *
 * 功能：
 *   记住每台设备的状态
 *   改状态时顺手动硬件
 *   改完通知界面和手机
 *   给灯带上色
 *   一键全关
 *   把状态拼成 JSON
 *   名字和设备互相查
 */
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "device_model.h"
#include "automation.h"

#include "led.h"
#include "fan.h"
#include "servo.h"

static const char *TAG = "device_model";

/* 功能：是不是四路灯带之一 */
static bool dev_is_led(device_id_t id)
{
    return (id == DEV_LED_LIVING) || (id == DEV_LED_KITCHEN) ||
           (id == DEV_LED_BEDROOM) || (id == DEV_LED_BATH);
}

/* 功能：是不是舵机类，窗门帘 */
static bool dev_is_servo(device_id_t id)
{
    return (id == DEV_WINDOW) || (id == DEV_DOOR) || (id == DEV_CURTAIN);
}

/* 功能：编号越界就拒绝 */
static bool dev_is_valid(device_id_t id)
{
    return ((int)id >= 0) && (id < DEV_COUNT);
}

/* 功能：设备换成灯带分区 */
static led_zone_t dev_to_led_zone(device_id_t id)
{
    switch (id) {
    case DEV_LED_LIVING:  return LED_ZONE_LIVING;   /* 功能：一路路对照，不靠顺序 */
    case DEV_LED_KITCHEN: return LED_ZONE_KITCHEN;
    case DEV_LED_BEDROOM: return LED_ZONE_BEDROOM;
    case DEV_LED_BATH:    return LED_ZONE_BATH;
    default:              return LED_ZONE_MAX;      /* 功能：不是灯 */
    }
}

/* 功能：设备换成舵机编号 */
static servo_id_t dev_to_servo_id(device_id_t id)
{
    switch (id) {
    case DEV_WINDOW:  return SERVO_WINDOW;   /* 功能：顺序不同，不能硬转 */
    case DEV_DOOR:    return SERVO_DOOR;
    case DEV_CURTAIN: return SERVO_CURTAIN;
    default:          return SERVO_MAX;      /* 功能：不是舵机 */
    }
}

/* 功能：全屋设备状态，锁护着 */
static device_state_t s_dev[DEV_COUNT];

/* 功能：舵机开合位置，0关100开 */
static uint8_t s_servo_pos[DEV_COUNT];

/* 功能：只留最后登记的回调 */
static device_event_cb_t s_cb      = NULL;
static void             *s_cb_user = NULL;

/* 功能：一把锁护住状态表 */
static SemaphoreHandle_t s_lock = NULL;

static bool s_inited = false;

static inline void dev_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void dev_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

/* 功能：统一回调，锁外再调 */
static void notify(device_id_t id, ctrl_source_t src)
{
    device_event_cb_t cb;
    void             *user;

    /* 功能：锁里只抄一份，锁外调 */
    dev_lock();
    cb   = s_cb;
    user = s_cb_user;
    dev_unlock();

    if (cb != NULL) {
        cb(id, src, user);
    }
}

/* 功能：是不是人手动改的 */
static bool src_is_manual(ctrl_source_t src)
{
    return (src == SRC_MQTT) || (src == SRC_BLE) ||
           (src == SRC_VOICE) || (src == SRC_LOCAL_KEY);
}

/* 功能：人改过就告诉联动 */
static void notify_manual_if_needed(device_id_t id, ctrl_source_t src)
{
    if (src_is_manual(src)) {
        automation_notify_manual(id);
    }
}

/* 功能：开关真硬件 */
static void hw_set_power(device_id_t id, bool on)
{
    esp_err_t err = ESP_OK;

    if (dev_is_led(id)) {
        err = led_set_power(dev_to_led_zone(id), on);
    } else if (id == DEV_FAN) {
        err = fan_set_power(on);
    } else if (dev_is_servo(id)) {
        err = servo_set_percent(dev_to_servo_id(id), on ? 100 : 0);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s hw_set_power(%d) failed: %s", device_id_name(id), (int)on, esp_err_to_name(err));
    }
}

/* 功能：调档位，也动硬件 */
static void hw_set_level(device_id_t id, uint8_t percent)
{
    esp_err_t err = ESP_OK;

    if (dev_is_led(id)) {
        led_zone_t zone = dev_to_led_zone(id);
        err = led_set_power(zone, percent > 0);
        if (err == ESP_OK) {
            err = led_set_brightness(zone, percent);
        }
    } else if (id == DEV_FAN) {
        err = fan_set_speed(percent);
    } else if (dev_is_servo(id)) {
        err = servo_set_percent(dev_to_servo_id(id), percent);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s hw_set_level(%u) failed: %s", device_id_name(id), (unsigned)percent, esp_err_to_name(err));
    }
}

/* 功能：给灯带上色 */
static void hw_set_rgb(device_id_t id, uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err = led_set_rgb(dev_to_led_zone(id), r, g, b);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s hw_set_rgb(%u,%u,%u) failed: %s", device_id_name(id),
                 (unsigned)r, (unsigned)g, (unsigned)b, esp_err_to_name(err));
    }
}

esp_err_t device_model_init(void)
{
    /* 功能：已经弄过就直接返回 */
    if (s_inited) {
        ESP_LOGD(TAG, "already initialized");
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            /* 功能：内存不够就不加锁 */
            ESP_LOGW(TAG, "mutex create failed, run without lock");
        }
    }

    /* 功能：默认全部先设成关 */
    for (int i = 0; i < DEV_COUNT; i++) {
        memset(&s_dev[i], 0, sizeof(s_dev[i]));
        s_dev[i].power      = false;
        s_dev[i].change_cnt = 0;
        s_servo_pos[i]      = 0;   /* 功能：窗门帘都归零 */

        if (dev_is_led((device_id_t)i)) {
            s_dev[i].level = 100;                          /* 功能：灯默认亮度满 */
            s_dev[i].r = s_dev[i].g = s_dev[i].b = 255;    /* 功能：灯默认白色 */
        } else {
            s_dev[i].level = 0;                            /* 功能：风扇舵机都归零 */
        }
    }

    /* 功能：把硬件准备好 */
    esp_err_t err = led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_init failed: %s (continue)", esp_err_to_name(err));
    }
    err = servo_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "servo_init failed: %s (continue)", esp_err_to_name(err));
    }
    err = fan_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fan_init failed: %s (continue)", esp_err_to_name(err));
    }

    /* 功能：把默认状态刷进硬件 */
    dev_lock();
    for (int i = 0; i < DEV_COUNT; i++) {
        hw_set_power((device_id_t)i, false);
    }
    dev_unlock();

    s_inited = true;
    ESP_LOGI(TAG, "device model ready, %d devices (all off, LED default 100%% white)",
             (int)DEV_COUNT);
    return ESP_OK;
}

esp_err_t device_set_power(device_id_t id, bool on, ctrl_source_t src)
{
    if (!dev_is_valid(id)) {
        ESP_LOGE(TAG, "set_power: bad id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    /* 功能：舵机开关就是转到头 */
    if (dev_is_servo(id)) {
        return device_set_level(id, on ? 100 : 0, src);
    }

    dev_lock();
    hw_set_power(id, on);
    s_dev[id].power = on;
    if (id == DEV_FAN) {
        /* 功能：风扇开关就是零或满 */
        s_dev[id].level = on ? 100 : 0;
    }
    /* 功能：开关不动灯的亮度 */
    s_dev[id].change_cnt++;
    uint8_t lvl = s_dev[id].level;      /* 功能：锁里抄一份打日志 */
    dev_unlock();

    notify_manual_if_needed(id, src);
    ESP_LOGI(TAG, "%s -> power=%d (level=%u) by %s",
             device_id_name(id), (int)on, (unsigned)lvl, ctrl_source_name(src));
    notify(id, src);
    return ESP_OK;
}

esp_err_t device_toggle(device_id_t id, ctrl_source_t src)
{
    if (!dev_is_valid(id)) {
        ESP_LOGE(TAG, "toggle: bad id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    /* 功能：读一下再反过来设 */
    bool cur = device_get_power(id);
    return device_set_power(id, !cur, src);
}

esp_err_t device_set_level(device_id_t id, uint8_t percent, ctrl_source_t src)
{
    if (!dev_is_valid(id)) {
        ESP_LOGE(TAG, "set_level: bad id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    if (percent > 100) {
        percent = 100;   /* 功能：最多只到一百 */
    }

    dev_lock();
    hw_set_level(id, percent);
    s_dev[id].level = percent;
    if (dev_is_servo(id)) {
        s_servo_pos[id] = percent;
        s_dev[id].power = (percent >= 50);          /* 功能：过半就算开着 */
    } else {
        s_dev[id].power = (percent > 0);            /* 功能：有档位就算开着 */
    }
    s_dev[id].change_cnt++;
    bool power = s_dev[id].power;       /* 功能：锁里抄一份打日志 */
    dev_unlock();

    notify_manual_if_needed(id, src);
    ESP_LOGI(TAG, "%s -> level=%u power=%d by %s",
             device_id_name(id), (unsigned)percent, (int)power, ctrl_source_name(src));
    notify(id, src);
    return ESP_OK;
}

esp_err_t device_set_color(device_id_t id, uint8_t r, uint8_t g, uint8_t b, ctrl_source_t src)
{
    if (!dev_is_valid(id) || !dev_is_led(id)) {
        /* 功能：只有灯有颜色 */
        ESP_LOGW(TAG, "set_color: %d is not an LED", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    dev_lock();
    hw_set_rgb(id, r, g, b);
    s_dev[id].r = r;
    s_dev[id].g = g;
    s_dev[id].b = b;
    s_dev[id].change_cnt++;     /* 功能：改色也算改过一回 */
    dev_unlock();

    /* 功能：改色也算人动过手 */
    notify_manual_if_needed(id, src);
    ESP_LOGI(TAG, "%s -> rgb(%u,%u,%u) by %s", device_id_name(id),
             (unsigned)r, (unsigned)g, (unsigned)b, ctrl_source_name(src));
    notify(id, src);
    return ESP_OK;
}

esp_err_t device_all_off(ctrl_source_t src)
{
    dev_lock();

    for (int i = 0; i < DEV_COUNT; i++) {
        device_id_t id = (device_id_t)i;

        if (dev_is_led(id)) {
            /* 功能：只关电源留住颜色 */
            hw_set_power(id, false);
            s_dev[i].power = false;
            s_dev[i].change_cnt++;
        } else if (id == DEV_FAN) {
            hw_set_level(id, 0);
            s_dev[i].power = false;
            s_dev[i].level = 0;
            s_dev[i].change_cnt++;
        } else if (dev_is_servo(id)) {
            /* 功能：窗门帘都关回去 */
            hw_set_level(id, 0);
            s_servo_pos[i]  = 0;
            s_dev[i].power  = false;
            s_dev[i].level  = 0;
            s_dev[i].change_cnt++;
        }
    }

    /* 功能：四路灯最后一起刷 */
    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_flush failed: %s", esp_err_to_name(err));
    }

    dev_unlock();

    /* 功能：人全关也得护一会儿 */
    if (src_is_manual(src)) {
        for (int i = 0; i < DEV_COUNT; i++) {
            automation_notify_manual((device_id_t)i);
        }
    }

    /* 功能：每台设备只通知一次 */
    for (int i = 0; i < DEV_COUNT; i++) {
        notify((device_id_t)i, src);
    }

    ESP_LOGI(TAG, "all off by %s", ctrl_source_name(src));
    return ESP_OK;
}

bool device_get_power(device_id_t id)
{
    if (!dev_is_valid(id)) {
        return false;
    }

    dev_lock();
    bool power = s_dev[id].power;
    dev_unlock();
    return power;
}

uint8_t device_get_level(device_id_t id)
{
    if (!dev_is_valid(id)) {
        return 0;
    }

    dev_lock();
    uint8_t level = s_dev[id].level;
    dev_unlock();
    return level;
}

esp_err_t device_get_color(device_id_t id, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!dev_is_valid(id) || !dev_is_led(id) || (r == NULL) || (g == NULL) || (b == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    dev_lock();
    *r = s_dev[id].r;
    *g = s_dev[id].g;
    *b = s_dev[id].b;
    dev_unlock();
    return ESP_OK;
}

const device_state_t *device_get_state(device_id_t id)
{
    if (!dev_is_valid(id)) {
        return NULL;
    }
    /* 功能：直接给出里头地址 */
    return &s_dev[id];
}

/* 功能：顺序得和设备清单一样 */
static const char *const s_dev_names[DEV_COUNT] = {
    "led_living",   /* 功能：客厅灯带 */
    "led_kitchen",  /* 功能：厨房灯带 */
    "led_bedroom",  /* 功能：卧室灯带 */
    "led_bath",     /* 功能：浴室灯带 */
    "fan",          /* 功能：风扇 */
    "window",       /* 功能：窗户 */
    "door",         /* 功能：门 */
    "curtain",      /* 功能：窗帘 */
};
_Static_assert(sizeof(s_dev_names) / sizeof(s_dev_names[0]) == DEV_COUNT,
               "device name table size must match DEV_COUNT");
_Static_assert(DEV_COUNT == 8, "device_id_t changed: update s_dev_names and the docs");

const char *device_id_name(device_id_t id)
{
    if (!dev_is_valid(id)) {
        return "unknown";
    }
    return s_dev_names[id];
}

/* 功能：名字反查设备 */
device_id_t device_from_name(const char *name)
{
    if (name == NULL) {
        return DEV_COUNT;
    }

    if (strcmp(name, "all") == 0) {
        return DEV_COUNT;       /* 功能：全部 */
    }

    for (int i = 0; i < DEV_COUNT; i++) {
        if (strcmp(name, s_dev_names[i]) == 0) {
            return (device_id_t)i;
        }
    }
    return DEV_COUNT;           /* 功能：找不到 */
}

/* 功能：拼一台设备的 JSON */
static void add_device_json(cJSON *root, device_id_t id, const device_state_t *st)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return;
    }

    (void)cJSON_AddBoolToObject(obj, "power", st->power);
    (void)cJSON_AddNumberToObject(obj, "level", (double)st->level);

    if (dev_is_led(id)) {
        (void)cJSON_AddNumberToObject(obj, "r", (double)st->r);
        (void)cJSON_AddNumberToObject(obj, "g", (double)st->g);
        (void)cJSON_AddNumberToObject(obj, "b", (double)st->b);
    }

    (void)cJSON_AddItemToObject(root, s_dev_names[id], obj);
}

int device_snapshot_json(char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0)) {
        ESP_LOGW(TAG, "snapshot: bad buffer");
        return 0;
    }

    /* 功能：锁里抄完就放锁 */
    device_state_t snap[DEV_COUNT];
    dev_lock();
    memcpy(snap, s_dev, sizeof(snap));
    dev_unlock();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "snapshot: cJSON_CreateObject failed");
        return 0;
    }

    for (int i = 0; i < DEV_COUNT; i++) {
        add_device_json(root, (device_id_t)i, &snap[i]);
    }

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);                 /* 功能：js 是自己一块内存 */
    if (js == NULL) {
        ESP_LOGE(TAG, "snapshot: cJSON_PrintUnformatted failed");
        return 0;
    }

    size_t need = strlen(js) + 1;       /* 功能：算上结尾那个零 */
    if (need > len) {
        ESP_LOGW(TAG, "snapshot buffer too small: need %u, have %u", (unsigned)need, (unsigned)len);
        cJSON_free(js);
        return 0;
    }

    memcpy(buf, js, need);
    int written = (int)(need - 1);      /* 功能：返回写了几个字 */
    cJSON_free(js);
    return written;
}

esp_err_t device_register_cb(device_event_cb_t cb, void *user_data)
{
    dev_lock();
    s_cb      = cb;         /* 功能：只留最后一个，空是注销 */
    s_cb_user = user_data;
    dev_unlock();

    ESP_LOGI(TAG, "event cb %s", (cb != NULL) ? "registered" : "cleared");
    return ESP_OK;
}

/* 功能：顺序得和来源清单一样 */
static const char *const s_src_names[] = {
    "boot",     /* 功能：上电 */
    "key",      /* 功能：按键 */
    "voice",    /* 功能：语音 */
    "mqtt",     /* 功能：联网手机 */
    "ble",      /* 功能：蓝牙手机 */
    "auto",     /* 功能：自动 */
    "selftest", /* 功能：自检 */
};
_Static_assert(sizeof(s_src_names) / sizeof(s_src_names[0]) == 7,
               "ctrl_source name table size must match ctrl_source_t");

const char *ctrl_source_name(ctrl_source_t src)
{
    if (((int)src < 0) || ((int)src >= (int)(sizeof(s_src_names) / sizeof(s_src_names[0])))) {
        return "unknown";
    }
    return s_src_names[src];
}
