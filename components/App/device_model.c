/**
 * @file  device_model.c
 * @brief 设备模型实现 —— 全屋设备的统一状态机（App 层核心）
 *
 * 设计要点：
 *   ① 所有控制入口（语音 / 按键 / MQTT / 自动联动）只调用本模块的 set 接口；
 *   ② 本模块负责「改软件状态 → 调 BSP 真正操作硬件 → 回调通知上层」三步；
 *   ③ 每次操作都带上 ctrl_source_t，说明"谁改的"；
 *   ④ 状态数组 s_dev[] 由一把 FreeRTOS 互斥锁保护（按键任务 / MQTT 任务 /
 *      主循环都会调用本模块），**回调统一在解锁之后触发**，绝不在持锁时回调。
 *
 * 【并发说明】
 *   本模块用 s_lock 保护 s_dev[] / s_servo_pos[]，避免多任务同时写导致字段撕裂；
 *   代价是每次 set 会短暂持锁（硬件写入在锁内，保证"状态与硬件动作"成对出现）。
 *   BSP 的 set 接口都是快速返回的（led 走 RMT 发送、fan/servo 只改 LEDC 占空比），
 *   不涉及长阻塞，所以持锁时间可控。若以后 BSP 里出现阻塞式操作（例如
 *   servo_set_angle_smooth 会 delay），应改为"锁内只改状态、锁外再打硬件"。
 *
 * 【为什么 App 层不出现任何 GPIO】
 *   所有引脚都在 components/BSP/board_config.h 里，App 只认 BSP 的语义接口。
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
/* automation.h 内部已经 include 了 device_model.h，靠 include guard 防循环包含 */
#include "automation.h"

/* ---------------- BSP 层驱动（只认语义接口，不含任何 GPIO 号） ---------------- */
#include "led.h"
#include "fan.h"
#include "servo.h"

static const char *TAG = "device_model";

/* ========================================================================= */
/*  设备 → BSP 映射                                                          */
/* ========================================================================= */
/*
 * ⚠ 关于枚举顺序的说明：
 *   device_model.h 里 DEV_LED_LIVING/KITCHEN/BEDROOM/BATH 的顺序，
 *   恰好和 led.h 里 LED_ZONE_LIVING/KITCHEN/BEDROOM/BATH 的顺序一致；
 *   但【不要依赖这个巧合】—— 这里显式写出每一路映射，以后任意一边调整顺序
 *   都不会静默错位。
 *
 *   舵机三个枚举的顺序更是【完全不一致】：
 *     device_model.h : DEV_WINDOW(5) → DEV_DOOR(6) → DEV_CURTAIN(7)
 *     servo.h        : SERVO_CURTAIN(0) → SERVO_WINDOW(1) → SERVO_DOOR(2)
 *   所以舵机绝对不能做强制类型转换，必须逐项显式映射。
 *
 *      DEV_LED_LIVING  → LED_ZONE_LIVING
 *      DEV_LED_KITCHEN → LED_ZONE_KITCHEN
 *      DEV_LED_BEDROOM → LED_ZONE_BEDROOM
 *      DEV_LED_BATH    → LED_ZONE_BATH
 *      DEV_FAN         → fan.h 的全局接口 fan_set_power / fan_set_speed
 *      DEV_WINDOW      → SERVO_WINDOW
 *      DEV_DOOR        → SERVO_DOOR
 *      DEV_CURTAIN     → SERVO_CURTAIN
 */

/** 设备是否为 4 路灯带之一 */
static bool dev_is_led(device_id_t id)
{
    return (id == DEV_LED_LIVING) || (id == DEV_LED_KITCHEN) ||
           (id == DEV_LED_BEDROOM) || (id == DEV_LED_BATH);
}

/** 设备是否为舵机类（窗 / 门 / 帘，用 level 表示 0~100 开合位置） */
static bool dev_is_servo(device_id_t id)
{
    return (id == DEV_WINDOW) || (id == DEV_DOOR) || (id == DEV_CURTAIN);
}

/** id 是否合法（越界一律安全拒绝） */
static bool dev_is_valid(device_id_t id)
{
    return ((int)id >= 0) && (id < DEV_COUNT);
}

/** 设备 → 灯带分区；非灯类返回 LED_ZONE_MAX（调用方必须先 dev_is_led 判断） */
static led_zone_t dev_to_led_zone(device_id_t id)
{
    switch (id) {
    case DEV_LED_LIVING:  return LED_ZONE_LIVING;   /* 显式映射，不靠枚举顺序巧合 */
    case DEV_LED_KITCHEN: return LED_ZONE_KITCHEN;
    case DEV_LED_BEDROOM: return LED_ZONE_BEDROOM;
    case DEV_LED_BATH:    return LED_ZONE_BATH;
    default:              return LED_ZONE_MAX;      /* 非灯类 */
    }
}

/** 设备 → 舵机编号；非舵机类返回 SERVO_MAX（调用方必须先 dev_is_servo 判断） */
static servo_id_t dev_to_servo_id(device_id_t id)
{
    switch (id) {
    case DEV_WINDOW:  return SERVO_WINDOW;   /* 顺序和 servo_id_t 不同，必须显式映射 */
    case DEV_DOOR:    return SERVO_DOOR;
    case DEV_CURTAIN: return SERVO_CURTAIN;
    default:          return SERVO_MAX;      /* 非舵机类 */
    }
}

/* ========================================================================= */
/*  内部状态                                                                 */
/* ========================================================================= */

/** 全屋设备状态（受 s_lock 保护） */
static device_state_t s_dev[DEV_COUNT];

/**
 * 舵机类设备单独记录的"开合位置" 0~100（受 s_lock 保护）。
 * 语义：0 = 关窗/关门/合帘，100 = 全开。
 * 它和 s_dev[id].level 始终同步，独立保存是为了让"位置"这个概念在代码里显式可见，
 * 也方便以后舵机需要额外记录目标角度/行程时扩展。
 */
static uint8_t s_servo_pos[DEV_COUNT];

/** 状态变化回调：只保留最后一次注册的 */
static device_event_cb_t s_cb      = NULL;
static void             *s_cb_user = NULL;

/** 状态互斥锁（device_model_init 里创建） */
static SemaphoreHandle_t s_lock = NULL;

static bool s_inited = false;

/* ========================================================================= */
/*  锁与回调辅助                                                             */
/* ========================================================================= */

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

/**
 * @brief 统一回调入口（★ 必须在释放锁之后调用）
 */
static void notify(device_id_t id, ctrl_source_t src)
{
    device_event_cb_t cb;
    void             *user;

    /* 只在锁内"取快照"，回调本身在锁外执行，避免回调里再调用本模块造成死锁 */
    dev_lock();
    cb   = s_cb;
    user = s_cb_user;
    dev_unlock();

    if (cb != NULL) {
        cb(id, src, user);
    }
}

/** 手动来源（需要启动自动联动的手动保护期）
 *  ⚠ BLE 必须算手动来源：否则手机通过蓝牙开的灯会被自动联动立刻覆盖。 */
static bool src_is_manual(ctrl_source_t src)
{
    return (src == SRC_MQTT) || (src == SRC_BLE) ||
           (src == SRC_VOICE) || (src == SRC_LOCAL_KEY);
}

/**
 * @brief 手动操作后通知自动联动模块（开 3 秒保护期）
 * @note 必须在 dev_unlock() 之后调用：automation_notify_manual 内部有自己的锁，
 *       放在本模块的锁内会有"锁嵌套 / 顺序反转"风险。
 */
static void notify_manual_if_needed(device_id_t id, ctrl_source_t src)
{
    if (src_is_manual(src)) {
        automation_notify_manual(id);
    }
}

/* ========================================================================= */
/*  纯硬件操作（只碰 BSP，不改 s_dev[]，不加锁 —— 由调用方持锁）               */
/* ========================================================================= */

/**
 * @brief 开关硬件
 * @note  LED   → led_set_power(zone, on)
 *        FAN   → fan_set_power(on)，on 等价 100% 转速
 *        舵机  → servo_set_percent(sid, on ? 100 : 0)
 * @note BSP 返回错误只告警，不改本函数的返回值：
 *       软件状态依然生效（没插硬件的开发阶段也能正常联调 MQTT / OLED）。
 */
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

/**
 * @brief 设置 0~100 档位（亮度 / 转速 / 开合位置）
 * @note  LED ：先 led_set_power(zone, percent > 0) 让硬件明确上/断电，
 *             再 led_set_brightness(zone, percent)。
 *             led.h 里亮度只是"对 RGB 做缩放"，若灯带处于关闭态，单设亮度不等于点亮，
 *             所以这里显式置一次电源位，保证"set 60 之后灯是亮的"。
 *        FAN ：fan_set_speed(percent)，0 会真正输出 0 占空比
 *        舵机：servo_set_percent(sid, percent)，内部直接转（不阻塞）
 */
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

/** @brief 设置灯带颜色（只对 DEV_LED_* 有效，调用方先判断） */
static void hw_set_rgb(device_id_t id, uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err = led_set_rgb(dev_to_led_zone(id), r, g, b);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s hw_set_rgb(%u,%u,%u) failed: %s", device_id_name(id),
                 (unsigned)r, (unsigned)g, (unsigned)b, esp_err_to_name(err));
    }
}

/* ========================================================================= */
/*  初始化                                                                   */
/* ========================================================================= */

esp_err_t device_model_init(void)
{
    /* 幂等：重复调用直接返回 */
    if (s_inited) {
        ESP_LOGD(TAG, "already initialized");
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            /* 极端内存不足：降级为"无锁"运行，功能仍然可用 */
            ESP_LOGW(TAG, "mutex create failed, run without lock");
        }
    }

    /* ---- ① 默认软件状态：全部关闭 ---- */
    for (int i = 0; i < DEV_COUNT; i++) {
        memset(&s_dev[i], 0, sizeof(s_dev[i]));
        s_dev[i].power      = false;
        s_dev[i].change_cnt = 0;
        s_servo_pos[i]      = 0;   /* 关窗 / 关门 / 合帘 */

        if (dev_is_led((device_id_t)i)) {
            s_dev[i].level = 100;                          /* LED 默认亮度 100 */
            s_dev[i].r = s_dev[i].g = s_dev[i].b = 255;    /* LED 默认全白 */
        } else {
            s_dev[i].level = 0;                            /* 风扇 0 转速、舵机 0 位置 */
        }
    }

    /* ---- ② BSP 初始化：容错，某一路失败只告警，不影响其它外设 ---- */
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

    /* ---- ③ 把默认状态真正刷到硬件（全部关闭 / 舵机归位） ----
     * 这里不走 set 接口：此时回调还没注册、change_cnt 保持 0 更干净。
     */
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

/* ========================================================================= */
/*  控制接口                                                                 */
/* ========================================================================= */

esp_err_t device_set_power(device_id_t id, bool on, ctrl_source_t src)
{
    if (!dev_is_valid(id)) {
        ESP_LOGE(TAG, "set_power: bad id %d", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    /* 舵机类语义：on = 转到 100%（全开），off = 回到 0%（全关），统一走 level 路径 */
    if (dev_is_servo(id)) {
        return device_set_level(id, on ? 100 : 0, src);
    }

    dev_lock();
    hw_set_power(id, on);
    s_dev[id].power = on;
    if (id == DEV_FAN) {
        /* 风扇的 power 与 level 是同一件事：on = 100% 转速，off = 0 */
        s_dev[id].level = on ? 100 : 0;
    }
    /* LED 的 level（亮度）在开关时保持不变：关灯后再开灯沿用原亮度 */
    s_dev[id].change_cnt++;
    uint8_t lvl = s_dev[id].level;      /* 锁内取快照，供锁外打日志 */
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

    /* 读 → 取反 → 走 set_power（读和写在两把锁之间，极端并发下可能丢一次翻转，
     * 属于可接受的低概率事件；严格场景请在上层串行化） */
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
        percent = 100;   /* 钳位 0~100（uint8_t 不可能小于 0） */
    }

    dev_lock();
    hw_set_level(id, percent);
    s_dev[id].level = percent;
    if (dev_is_servo(id)) {
        s_servo_pos[id] = percent;
        s_dev[id].power = (percent >= 50);          /* 舵机：位置 >= 50% 视为"开" */
    } else {
        s_dev[id].power = (percent > 0);            /* LED / 风扇：有档位就是开 */
    }
    s_dev[id].change_cnt++;
    bool power = s_dev[id].power;       /* 锁内取快照，供锁外打日志 */
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
        /* 只有灯带才有颜色，风扇 / 舵机一律拒绝 */
        ESP_LOGW(TAG, "set_color: %d is not an LED", (int)id);
        return ESP_ERR_INVALID_ARG;
    }

    dev_lock();
    hw_set_rgb(id, r, g, b);
    s_dev[id].r = r;
    s_dev[id].g = g;
    s_dev[id].b = b;
    s_dev[id].change_cnt++;     /* 颜色变化也算一次状态变化 */
    dev_unlock();

    /* 颜色变化同样算"手动操作"，避免自动规则紧接着改灯的状态 */
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
            /* 灯：只关电源，保留亮度/颜色（下次开灯沿用） */
            hw_set_power(id, false);
            s_dev[i].power = false;
            s_dev[i].change_cnt++;
        } else if (id == DEV_FAN) {
            hw_set_level(id, 0);
            s_dev[i].power = false;
            s_dev[i].level = 0;
            s_dev[i].change_cnt++;
        } else if (dev_is_servo(id)) {
            /* 舵机归位：关窗 / 关门 / 合帘 */
            hw_set_level(id, 0);
            s_servo_pos[i]  = 0;
            s_dev[i].power  = false;
            s_dev[i].level  = 0;
            s_dev[i].change_cnt++;
        }
    }

    /* 4 路灯带是批量改的，最后统一刷一次，避免逐路刷屏闪 */
    esp_err_t err = led_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_flush failed: %s", esp_err_to_name(err));
    }

    dev_unlock();

    /* 手动"全部关闭"也要开保护期，否则自动规则会立刻把灯/窗又打开 */
    if (src_is_manual(src)) {
        for (int i = 0; i < DEV_COUNT; i++) {
            automation_notify_manual((device_id_t)i);
        }
    }

    /* 批量改完后，每个设备只回调一次（避免上层被刷屏） */
    for (int i = 0; i < DEV_COUNT; i++) {
        notify((device_id_t)i, src);
    }

    ESP_LOGI(TAG, "all off by %s", ctrl_source_name(src));
    return ESP_OK;
}

/* ========================================================================= */
/*  查询接口                                                                 */
/* ========================================================================= */

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
    /* 返回内部数组元素的地址（只读约定，调用方不得修改） */
    return &s_dev[id];
}

/* ========================================================================= */
/*  名字表                                                                   */
/* ========================================================================= */

/** ★ 顺序必须和 device_model.h 里 device_id_t 的枚举顺序严格一致 */
static const char *const s_dev_names[DEV_COUNT] = {
    "led_living",   /* DEV_LED_LIVING  = 0 */
    "led_kitchen",  /* DEV_LED_KITCHEN = 1 */
    "led_bedroom",  /* DEV_LED_BEDROOM = 2 */
    "led_bath",     /* DEV_LED_BATH    = 3 */
    "fan",          /* DEV_FAN         = 4 */
    "window",       /* DEV_WINDOW      = 5 */
    "door",         /* DEV_DOOR        = 6 */
    "curtain",      /* DEV_CURTAIN     = 7 */
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

/**
 * @brief 名字反查设备
 * @return 命中的 device_id_t；
 *         ★ 约定：名字为 "all" 时返回 DEV_COUNT（表示"全部设备"，由调用方识别）；
 *           名字识别不出来时同样返回 DEV_COUNT（它不是合法 id，调用方用
 *           `id >= DEV_COUNT` 即可统一判为"非法 / 全部"）。
 *           这样上层只需要一种边界判断，不用引入额外的错误码。
 */
device_id_t device_from_name(const char *name)
{
    if (name == NULL) {
        return DEV_COUNT;
    }

    if (strcmp(name, "all") == 0) {
        return DEV_COUNT;       /* 全部（由 mqtt_app / voice_app 识别处理） */
    }

    for (int i = 0; i < DEV_COUNT; i++) {
        if (strcmp(name, s_dev_names[i]) == 0) {
            return (device_id_t)i;
        }
    }
    return DEV_COUNT;           /* 找不到 */
}

/* ========================================================================= */
/*  JSON 序列化                                                              */
/* ========================================================================= */

/**
 * @brief 组装单个设备的 JSON 对象
 * @note 结构严格对齐 mqtt_protocol.h 的「上行 state JSON」：
 *         灯  ：{"power":true,"level":80,"r":255,"g":255,"b":255}
 *         风扇：{"power":false,"level":0}
 *         舵机：{"power":false,"level":0}   ← level 即开合位置
 *       ★ 只有 LED 才带 r/g/b。
 */
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

    /* 锁内只做一次快照拷贝，JSON 组装放在锁外，缩短持锁时间 */
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
    cJSON_Delete(root);                 /* js 是独立分配的内存，删 root 不影响它 */
    if (js == NULL) {
        ESP_LOGE(TAG, "snapshot: cJSON_PrintUnformatted failed");
        return 0;
    }

    size_t need = strlen(js) + 1;       /* 含结尾 '\0' */
    if (need > len) {
        ESP_LOGW(TAG, "snapshot buffer too small: need %u, have %u", (unsigned)need, (unsigned)len);
        cJSON_free(js);
        return 0;
    }

    memcpy(buf, js, need);
    int written = (int)(need - 1);      /* 返回写入的字符数（不含 '\0'） */
    cJSON_free(js);
    return written;
}

/* ========================================================================= */
/*  事件订阅与来源名                                                          */
/* ========================================================================= */

esp_err_t device_register_cb(device_event_cb_t cb, void *user_data)
{
    dev_lock();
    s_cb      = cb;         /* 只保留最后一个；cb == NULL 表示注销 */
    s_cb_user = user_data;
    dev_unlock();

    ESP_LOGI(TAG, "event cb %s", (cb != NULL) ? "registered" : "cleared");
    return ESP_OK;
}

/** ★ 顺序必须和 device_model.h 里 ctrl_source_t 的枚举顺序严格一致 */
static const char *const s_src_names[] = {
    "boot",     /* SRC_BOOT      = 0 */
    "key",      /* SRC_LOCAL_KEY = 1 */
    "voice",    /* SRC_VOICE     = 2 */
    "mqtt",     /* SRC_MQTT      = 3 */
    "ble",      /* SRC_BLE       = 4 */
    "auto",     /* SRC_AUTO      = 5 */
    "selftest", /* SRC_SELFTEST  = 6 */
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
