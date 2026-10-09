#include "device_model.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "automation.h"

#include "led.h"
#include "fan.h"
#include "servo.h"

// ======================================================================
// 设备状态表：全屋设备都在这儿，谁改都走这里
//
// 对应 ESP-IDF 工程的 components/App/device_model.c。
// 原工程用互斥锁护着状态表，因为网络、蓝牙、主任务会同时进来改；
// 这版所有改动都从主循环进（网络和蓝牙的回调只把报文排队，
// 真正处理在主循环里做），所以不加锁也不会打架。
// ======================================================================

static const char *const TAG = "device_model";

// 是不是四路灯之一
static bool dev_is_led(device_id_t id) {
    return (id == DEV_LED_LIVING) || (id == DEV_LED_KITCHEN) ||
           (id == DEV_LED_BEDROOM) || (id == DEV_LED_BATH);
}

// 是不是舵机类，窗门帘
static bool dev_is_servo(device_id_t id) {
    return (id == DEV_WINDOW) || (id == DEV_DOOR) || (id == DEV_CURTAIN);
}

// 编号越界就拒绝
static bool dev_is_valid(device_id_t id) {
    return ((int)id >= 0) && (id < DEV_COUNT);
}

// 设备换成灯分区
static led_zone_t dev_to_led_zone(device_id_t id) {
    switch (id) {
    // 一路路对照，不靠顺序
    case DEV_LED_LIVING:  return LED_ZONE_LIVING;
    case DEV_LED_KITCHEN: return LED_ZONE_KITCHEN;
    case DEV_LED_BEDROOM: return LED_ZONE_BEDROOM;
    case DEV_LED_BATH:    return LED_ZONE_BATH;
    // 不是灯
    default:              return LED_ZONE_MAX;
    }
}

// 设备换成舵机编号
static servo_id_t dev_to_servo_id(device_id_t id) {
    switch (id) {
    // 顺序不同，不能硬转
    case DEV_WINDOW:  return SERVO_WINDOW;
    case DEV_DOOR:    return SERVO_DOOR;
    case DEV_CURTAIN: return SERVO_CURTAIN;
    // 不是舵机
    default:          return SERVO_MAX;
    }
}

// 全屋设备状态
static device_state_t s_dev[DEV_COUNT];

// 只留最后登记的回调
static device_event_cb_t s_cb      = NULL;
static void             *s_cb_user = NULL;

static bool s_inited = false;

// 统一回调
static void notify(device_id_t id, ctrl_source_t src) {
    if (s_cb != NULL) {
        s_cb(id, src, s_cb_user);
    }
}

// 是不是人手动改的
static bool src_is_manual(ctrl_source_t src) {
    return (src == SRC_MQTT) || (src == SRC_BLE) ||
           (src == SRC_VOICE) || (src == SRC_LOCAL_KEY);
}

// 人改过就告诉联动
static void notify_manual_if_needed(device_id_t id, ctrl_source_t src) {
    if (src_is_manual(src)) {
        automation_notify_manual(id);
    }
}

// 开关真硬件
static void hw_set_power(device_id_t id, bool on) {
    bool ok = true;

    if (dev_is_led(id)) {
        ok = led_set_power(dev_to_led_zone(id), on);
    } else if (id == DEV_FAN) {
        ok = fan_set_power(on);
    } else if (dev_is_servo(id)) {
        ok = servo_set_percent(dev_to_servo_id(id), on ? 100 : 0);
    }

    if (!ok) {
        Serial.printf("%s: %s hw_set_power(%d) 没成功\n", TAG, device_id_name(id), (int)on);
    }
}

// 调档位，也动硬件
static void hw_set_level(device_id_t id, uint8_t percent) {
    bool ok = true;

    if (dev_is_led(id)) {
        const led_zone_t zone = dev_to_led_zone(id);
        ok = led_set_power(zone, percent > 0);
        if (ok) {
            ok = led_set_brightness(zone, percent);
        }
    } else if (id == DEV_FAN) {
        ok = fan_set_speed(percent);
    } else if (dev_is_servo(id)) {
        ok = servo_set_percent(dev_to_servo_id(id), percent);
    }

    if (!ok) {
        Serial.printf("%s: %s hw_set_level(%u) 没成功\n", TAG,
                      device_id_name(id), (unsigned)percent);
    }
}

// 给灯改颜色，单色灯只是记状态
static void hw_set_rgb(device_id_t id, uint8_t r, uint8_t g, uint8_t b) {
    if (!dev_is_led(id)) {
        return;
    }

    led_set_rgb(dev_to_led_zone(id), r, g, b);
}

bool device_model_init(void) {
    if (s_inited) {
        return true;
    }

    // 默认全部先设成关
    for (int i = 0; i < DEV_COUNT; i++) {
        memset(&s_dev[i], 0, sizeof(s_dev[i]));
        s_dev[i].power = false;

        if (dev_is_led((device_id_t)i)) {
            // 灯默认亮度满
            s_dev[i].level = 100;
            // 灯默认白色
            s_dev[i].r = s_dev[i].g = s_dev[i].b = 255;
        } else {
            // 风扇舵机都归零
            s_dev[i].level = 0;
        }
    }

    // 把硬件准备好，缺件不算致命
    if (!led_init()) {
        Serial.printf("%s: led_init 失败（继续跑）\n", TAG);
    }
    if (!servo_init()) {
        Serial.printf("%s: servo_init 失败（继续跑）\n", TAG);
    }
    if (!fan_init()) {
        Serial.printf("%s: fan_init 失败（继续跑）\n", TAG);
    }

    // 把默认状态刷进硬件
    for (int i = 0; i < DEV_COUNT; i++) {
        hw_set_power((device_id_t)i, false);
    }

    s_inited = true;
    Serial.printf("%s: device model ready, %d devices (all off, LED default 100%% white)\n",
                  TAG, (int)DEV_COUNT);
    return true;
}

bool device_set_power(device_id_t id, bool on, ctrl_source_t src) {
    if (!dev_is_valid(id)) {
        Serial.printf("%s: set_power 设备号不对 %d\n", TAG, (int)id);
        return false;
    }

    // 舵机开关就是转到头
    if (dev_is_servo(id)) {
        return device_set_level(id, on ? 100 : 0, src);
    }

    hw_set_power(id, on);
    s_dev[id].power = on;
    if (id == DEV_FAN) {
        // 风扇开关就是零或满
        s_dev[id].level = on ? 100 : 0;
    }
    // 开关不动灯的亮度
    const uint8_t lvl = s_dev[id].level;

    notify_manual_if_needed(id, src);
    Serial.printf("%s: %s -> power=%d (level=%u) by %s\n",
                  TAG, device_id_name(id), (int)on, (unsigned)lvl, ctrl_source_name(src));
    notify(id, src);
    return true;
}

bool device_toggle(device_id_t id, ctrl_source_t src) {
    if (!dev_is_valid(id)) {
        Serial.printf("%s: toggle 设备号不对 %d\n", TAG, (int)id);
        return false;
    }

    // 读一下再反过来设
    const bool cur = device_get_power(id);
    return device_set_power(id, !cur, src);
}

bool device_set_level(device_id_t id, uint8_t percent, ctrl_source_t src) {
    if (!dev_is_valid(id)) {
        Serial.printf("%s: set_level 设备号不对 %d\n", TAG, (int)id);
        return false;
    }

    // 窗帘舵机写死不要了：命令直接拒掉，状态也别假装变了
    if (dev_is_servo(id) && !servo_is_enabled(dev_to_servo_id(id))) {
        Serial.printf("%s: %s 已停用，命令忽略\n", TAG, device_id_name(id));
        return false;
    }

    if (percent > 100) {
        // 最多只到一百
        percent = 100;
    }

    hw_set_level(id, percent);
    s_dev[id].level = percent;
    if (dev_is_servo(id)) {
        // 过半就算开着
        s_dev[id].power = (percent >= 50);
    } else {
        // 有档位就算开着
        s_dev[id].power = (percent > 0);
    }
    const bool power = s_dev[id].power;

    notify_manual_if_needed(id, src);
    Serial.printf("%s: %s -> level=%u power=%d by %s\n",
                  TAG, device_id_name(id), (unsigned)percent, (int)power, ctrl_source_name(src));
    notify(id, src);
    return true;
}

bool device_set_color(device_id_t id, uint8_t r, uint8_t g, uint8_t b, ctrl_source_t src) {
    if (!dev_is_valid(id) || !dev_is_led(id)) {
        // 只有灯有颜色
        Serial.printf("%s: set_color 第 %d 台不是灯\n", TAG, (int)id);
        return false;
    }

    hw_set_rgb(id, r, g, b);
    s_dev[id].r = r;
    s_dev[id].g = g;
    s_dev[id].b = b;

    // 改色也算人动过手
    notify_manual_if_needed(id, src);
    Serial.printf("%s: %s -> rgb(%u,%u,%u) by %s\n", TAG, device_id_name(id),
                  (unsigned)r, (unsigned)g, (unsigned)b, ctrl_source_name(src));
    notify(id, src);
    return true;
}

bool device_all_off(ctrl_source_t src) {
    for (int i = 0; i < DEV_COUNT; i++) {
        const device_id_t id = (device_id_t)i;

        if (dev_is_led(id)) {
            // 只关电源留住颜色
            hw_set_power(id, false);
            s_dev[i].power = false;
        } else if (id == DEV_FAN) {
            hw_set_level(id, 0);
            s_dev[i].power = false;
            s_dev[i].level = 0;
        } else if (dev_is_servo(id)) {
            // 窗门都关回去，窗帘停用了会自己跳过
            hw_set_level(id, 0);
            s_dev[i].power = false;
            s_dev[i].level = 0;
        }
    }

    // 四路灯最后一起刷
    led_flush();

    // 人全关也得护一会儿
    if (src_is_manual(src)) {
        for (int i = 0; i < DEV_COUNT; i++) {
            automation_notify_manual((device_id_t)i);
        }
    }

    // 每台设备只通知一次
    for (int i = 0; i < DEV_COUNT; i++) {
        notify((device_id_t)i, src);
    }

    Serial.printf("%s: all off by %s\n", TAG, ctrl_source_name(src));
    return true;
}

bool device_get_power(device_id_t id) {
    if (!dev_is_valid(id)) {
        return false;
    }
    return s_dev[id].power;
}

uint8_t device_get_level(device_id_t id) {
    if (!dev_is_valid(id)) {
        return 0;
    }
    return s_dev[id].level;
}

// 顺序得和设备清单一样
static const char *const s_dev_names[DEV_COUNT] = {
    // 客厅灯
    "led_living",
    // 厨房灯
    "led_kitchen",
    // 卧室灯
    "led_bedroom",
    // 浴室灯
    "led_bath",
    // 风扇
    "fan",
    // 窗户
    "window",
    // 门
    "door",
    // 窗帘
    "curtain",
};

static_assert(sizeof(s_dev_names) / sizeof(s_dev_names[0]) == DEV_COUNT,
              "device name table size must match DEV_COUNT");
static_assert(DEV_COUNT == 8, "device_id_t changed: update s_dev_names and the docs");

const char *device_id_name(device_id_t id) {
    if (!dev_is_valid(id)) {
        return "unknown";
    }
    return s_dev_names[id];
}

// 名字反查设备
device_id_t device_from_name(const char *name) {
    if (name == NULL) {
        return DEV_COUNT;
    }

    if (strcmp(name, "all") == 0) {
        // 全部
        return DEV_COUNT;
    }

    for (int i = 0; i < DEV_COUNT; i++) {
        if (strcmp(name, s_dev_names[i]) == 0) {
            return (device_id_t)i;
        }
    }
    // 找不到
    return DEV_COUNT;
}

// 往缓冲里贴一段，贴不下就返回剩下的长度
static int json_append(char *buf, size_t len, int used, const char *fmt, ...) {
    if (used >= (int)len) {
        return used;
    }

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + used, len - (size_t)used, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return used;
    }
    return used + n;
}

// 拼一台设备的 JSON
static int add_device_json(char *buf, size_t len, int used, device_id_t id,
                           const device_state_t *st) {
    if (dev_is_led(id)) {
        return json_append(buf, len, used,
                           "%s\"%s\":{\"power\":%s,\"level\":%u,\"r\":%u,\"g\":%u,\"b\":%u}",
                           (used > 1) ? "," : "",
                           device_id_name(id), st->power ? "true" : "false",
                           (unsigned)st->level, (unsigned)st->r, (unsigned)st->g, (unsigned)st->b);
    }

    return json_append(buf, len, used,
                       "%s\"%s\":{\"power\":%s,\"level\":%u}",
                       (used > 1) ? "," : "",
                       device_id_name(id), st->power ? "true" : "false",
                       (unsigned)st->level);
}

int device_snapshot_json(char *buf, size_t len) {
    if ((buf == NULL) || (len == 0)) {
        Serial.printf("%s: snapshot 缓冲区不对\n", TAG);
        return 0;
    }

    int used = json_append(buf, len, 0, "{");
    for (int i = 0; i < DEV_COUNT; i++) {
        used = add_device_json(buf, len, used, (device_id_t)i, &s_dev[i]);
    }
    used = json_append(buf, len, used, "}");

    if (used >= (int)len) {
        // 装不下就整条作废，别发出去半截 JSON
        Serial.printf("%s: snapshot 缓冲区太小（要 %d 字节，只有 %u）\n",
                      TAG, used, (unsigned)len);
        return 0;
    }

    return used;
}

// 登记状态变化的回调，只留最后一个
void device_register_cb(device_event_cb_t cb, void *user_data) {
    s_cb      = cb;
    s_cb_user = user_data;

    Serial.printf("%s: event cb %s\n", TAG, (cb != NULL) ? "registered" : "cleared");
}

// 顺序得和来源清单一样
static const char *const s_src_names[] = {
    // 上电
    "boot",
    // 按键
    "key",
    // 语音
    "voice",
    // 联网手机
    "mqtt",
    // 蓝牙手机
    "ble",
    // 自动
    "auto",
    // 自检
    "selftest",
};

static_assert(sizeof(s_src_names) / sizeof(s_src_names[0]) == 7,
              "ctrl_source name table size must match ctrl_source_t");

const char *ctrl_source_name(ctrl_source_t src) {
    if (((int)src < 0) ||
        ((int)src >= (int)(sizeof(s_src_names) / sizeof(s_src_names[0])))) {
        return "unknown";
    }
    return s_src_names[src];
}
