#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "device_model.h"
#include "esp_log.h"
#include "sensor.h"

static const char *TAG = "SELFTEST";

// 主循环半秒一拍
#define TEST_TICK_MS        500

// 一个测试项长这样
typedef struct {
    // 屏幕上显示的名字
    const char *name;
    // 开跑时干什么
    void      (*start)(void);
    // 收尾时把设备还原
    void      (*finish)(void);
    // 跑几拍，零就马上完
    uint8_t    run_ticks;
} test_item_t;

// 每项的开和收
static void t_led_living(void)  { device_set_power(DEV_LED_LIVING, true, SRC_SELFTEST); }
static void t_led_living_end(void)  { device_set_power(DEV_LED_LIVING, false, SRC_SELFTEST); }
static void t_led_kitchen(void) { device_set_power(DEV_LED_KITCHEN, true, SRC_SELFTEST); }
static void t_led_kitchen_end(void) { device_set_power(DEV_LED_KITCHEN, false, SRC_SELFTEST); }
static void t_led_bedroom(void) { device_set_power(DEV_LED_BEDROOM, true, SRC_SELFTEST); }
static void t_led_bedroom_end(void) { device_set_power(DEV_LED_BEDROOM, false, SRC_SELFTEST); }
static void t_led_bath(void)    { device_set_power(DEV_LED_BATH, true, SRC_SELFTEST); }
static void t_led_bath_end(void)    { device_set_power(DEV_LED_BATH, false, SRC_SELFTEST); }
static void t_fan(void)         { device_set_level(DEV_FAN, 60, SRC_SELFTEST); }
static void t_fan_end(void)     { device_set_power(DEV_FAN, false, SRC_SELFTEST); }
// 一半就是九十度
static void t_window(void)      { device_set_level(DEV_WINDOW, 50, SRC_SELFTEST); }
static void t_window_end(void)  { device_set_level(DEV_WINDOW, 0, SRC_SELFTEST); }
static void t_door(void)        { device_set_level(DEV_DOOR, 50, SRC_SELFTEST); }
static void t_door_end(void)    { device_set_level(DEV_DOOR, 0, SRC_SELFTEST); }
static void t_curtain(void)     { device_set_level(DEV_CURTAIN, 50, SRC_SELFTEST); }
static void t_curtain_end(void) { device_set_level(DEV_CURTAIN, 0, SRC_SELFTEST); }

#define TEST_ITEM_COUNT         9
// 末项是看读数
#define TEST_IDX_SENSOR         (TEST_ITEM_COUNT - 1)

static const test_item_t s_items[TEST_ITEM_COUNT] = {
    // 亮两秒
    { "客厅灯", t_led_living,  t_led_living_end,  4 },
    { "厨房灯", t_led_kitchen, t_led_kitchen_end, 4 },
    { "卧室灯", t_led_bedroom, t_led_bedroom_end, 4 },
    { "浴室灯", t_led_bath,    t_led_bath_end,    4 },
    // 转两秒六成风
    { "风扇",   t_fan,         t_fan_end,         4 },
    // 转过去停会儿再回
    { "窗户",   t_window,      t_window_end,      3 },
    { "门",     t_door,        t_door_end,        3 },
    { "窗帘",   t_curtain,     t_curtain_end,     3 },
    // 只看读数
    { "传感器", NULL,          NULL,              0 },
};

// 一项的三步
typedef enum {
    // 等着按
    PHASE_IDLE = 0,
    // 正在跑
    PHASE_RUN,
    // 跑完了
    PHASE_DONE,
} phase_t;

static bool     s_active    = false;
// 进来之前联动是开是关
static bool     s_prev_auto = true;
static uint8_t  s_idx       = 0;
static phase_t  s_phase     = PHASE_IDLE;
// 这一项跑了几拍
static uint16_t s_elapsed   = 0;

bool selftest_is_active(void)
{
    return s_active;
}

void selftest_enter(void)
{
    if (s_active) {
        return;
    }

    s_active    = true;
    s_prev_auto = automation_is_enabled();
    // 自检时先摁住联动
    automation_set_enabled(false);
    s_idx       = 0;
    s_phase     = PHASE_IDLE;
    s_elapsed   = 0;

    ESP_LOGI(TAG, "selftest mode ON (auto paused, was %s)",
             s_prev_auto ? "on" : "off");
}

void selftest_exit(void)
{
    if (!s_active) {
        return;
    }

    // 走之前先把设备收好
    const test_item_t *it = &s_items[s_idx];
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        it->finish();
    }

    // 把联动恢复原样
    automation_set_enabled(s_prev_auto);
    s_active = false;

    ESP_LOGI(TAG, "selftest mode OFF (auto restored to %s)",
             s_prev_auto ? "on" : "off");
    // 屏幕下一拍自己会刷
}

void selftest_run_current(void)
{
    if (!s_active) {
        return;
    }

    const test_item_t *it = &s_items[s_idx];

    // 重跑前先收个尾
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        it->finish();
    }

    s_phase   = PHASE_RUN;
    s_elapsed = 0;

    if (it->start != NULL) {
        it->start();
    }
    ESP_LOGI(TAG, "run item %d/%d: %s", (int)s_idx + 1, TEST_ITEM_COUNT, it->name);
}

void selftest_next(void)
{
    if (!s_active) {
        return;
    }

    const test_item_t *it = &s_items[s_idx];
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        // 切走前先收尾
        it->finish();
    }

    s_idx     = (uint8_t)((s_idx + 1) % TEST_ITEM_COUNT);
    s_phase   = PHASE_IDLE;
    s_elapsed = 0;

    ESP_LOGI(TAG, "item %d/%d: %s", (int)s_idx + 1, TEST_ITEM_COUNT, s_items[s_idx].name);
}

void selftest_prev(void)
{
    if (!s_active) {
        return;
    }

    const test_item_t *it = &s_items[s_idx];
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        // 切走前先收尾
        it->finish();
    }

    s_idx     = (uint8_t)((s_idx + TEST_ITEM_COUNT - 1) % TEST_ITEM_COUNT);
    s_phase   = PHASE_IDLE;
    s_elapsed = 0;

    ESP_LOGI(TAG, "item %d/%d: %s", (int)s_idx + 1, TEST_ITEM_COUNT, s_items[s_idx].name);
}

void selftest_on_adkey(adkey_id_t id, adkey_event_t ev)
{
    if (ev != ADKEY_EVENT_CLICK && ev != ADKEY_EVENT_LONG_PRESS) {
        // 按下抬起先不管
        return;
    }

    if (ev == ADKEY_EVENT_LONG_PRESS) {
        if (id == ADKEY_OK) {
            // 长按收工
            selftest_exit();
        }
        return;
    }

    switch (id) {
    case ADKEY_1:
        // 一号键退出
        selftest_exit();
        break;
    case ADKEY_3:
        // 三号键往上翻
        selftest_prev();
        break;
    case ADKEY_4:
        // 四号键往下翻
        selftest_next();
        break;
    case ADKEY_OK:
        // 确认键就开跑
        selftest_run_current();
        break;
    default:
        // 二号键先空着
        break;
    }
}

void selftest_tick(void)
{
    if (!s_active) {
        return;
    }

    if (s_phase == PHASE_RUN) {
        const test_item_t *it = &s_items[s_idx];
        s_elapsed++;
        if (s_elapsed >= it->run_ticks) {
            if (it->finish != NULL) {
                it->finish();
            }
            s_phase = PHASE_DONE;
            ESP_LOGI(TAG, "item %s done", it->name);
        }
    }

}

uint8_t selftest_get_count(void)
{
    return TEST_ITEM_COUNT;
}

uint8_t selftest_get_index(void)
{
    return s_idx;
}

uint8_t selftest_get_phase(void)
{
    return (uint8_t)s_phase;
}

const char *selftest_get_item_name(uint8_t idx)
{
    return (idx < TEST_ITEM_COUNT) ? s_items[idx].name : "";
}

void selftest_run_index(uint8_t idx)
{
    if (!s_active || idx >= TEST_ITEM_COUNT) {
        return;
    }
    s_idx = idx;
    selftest_run_current();
}
