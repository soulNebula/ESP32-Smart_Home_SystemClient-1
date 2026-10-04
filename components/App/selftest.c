/*
 * 模块：
 *   开机自检（怎么用见 selftest.h）。这儿只管排一遍流程：该哪一项了、
 *   跑了多久、跑完收尾；按下去就是去喊设备总状态表（device_model.c）
 *   动手，跟平时用户操作走同一条路，所以测出来的就是真本事。
 *   被 main.c 的主循环每半秒推一下；自己临时摁住自动联动
 *   （automation.c），退出时再放回去。
 *
 *   不自建常驻任务，跟着主循环的节拍走；屏幕上画由界面那块负责，
 *   本文件只管状态机。测试项上的字都挑常用字，免得屏幕上是方块。
 *
 * 功能：
 *   一项项点设备
 *   半秒推进一步
 *   跑完把设备收回去
 *   退出时恢复联动
 */
#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "device_model.h"
#include "esp_log.h"
#include "sensor.h"

static const char *TAG = "SELFTEST";

/* 功能：主循环半秒一拍 */
#define TEST_TICK_MS        500

/* 功能：一个测试项长这样 */
typedef struct {
    const char *name;               /* 功能：屏幕上显示的名字 */
    void      (*start)(void);       /* 功能：开跑时干什么 */
    void      (*finish)(void);      /* 功能：收尾时把设备还原 */
    uint8_t    run_ticks;           /* 功能：跑几拍，零就马上完 */
} test_item_t;

/* 功能：每项的开和收 */
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
static void t_window(void)      { device_set_level(DEV_WINDOW, 50, SRC_SELFTEST); }   /* 功能：一半就是九十度 */
static void t_window_end(void)  { device_set_level(DEV_WINDOW, 0, SRC_SELFTEST); }
static void t_door(void)        { device_set_level(DEV_DOOR, 50, SRC_SELFTEST); }
static void t_door_end(void)    { device_set_level(DEV_DOOR, 0, SRC_SELFTEST); }
static void t_curtain(void)     { device_set_level(DEV_CURTAIN, 50, SRC_SELFTEST); }
static void t_curtain_end(void) { device_set_level(DEV_CURTAIN, 0, SRC_SELFTEST); }

#define TEST_ITEM_COUNT         9
#define TEST_IDX_SENSOR         (TEST_ITEM_COUNT - 1)   /* 功能：末项是看读数 */

static const test_item_t s_items[TEST_ITEM_COUNT] = {
    { "客厅灯", t_led_living,  t_led_living_end,  4 },   /* 功能：亮两秒 */
    { "厨房灯", t_led_kitchen, t_led_kitchen_end, 4 },
    { "卧室灯", t_led_bedroom, t_led_bedroom_end, 4 },
    { "浴室灯", t_led_bath,    t_led_bath_end,    4 },
    { "风扇",   t_fan,         t_fan_end,         4 },   /* 功能：转两秒六成风 */
    { "窗户",   t_window,      t_window_end,      3 },   /* 功能：转过去停会儿再回 */
    { "门",     t_door,        t_door_end,        3 },
    { "窗帘",   t_curtain,     t_curtain_end,     3 },
    { "传感器", NULL,          NULL,              0 },   /* 功能：只看读数 */
};

/* 功能：一项的三步 */
typedef enum {
    PHASE_IDLE = 0,   /* 功能：等着按 */
    PHASE_RUN,        /* 功能：正在跑 */
    PHASE_DONE,       /* 功能：跑完了 */
} phase_t;

static bool     s_active    = false;
static bool     s_prev_auto = true;     /* 功能：进来之前联动是开是关 */
static uint8_t  s_idx       = 0;
static phase_t  s_phase     = PHASE_IDLE;
static uint16_t s_elapsed   = 0;        /* 功能：这一项跑了几拍 */

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
    automation_set_enabled(false);          /* 功能：自检时先摁住联动 */
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

    /* 功能：走之前先把设备收好 */
    const test_item_t *it = &s_items[s_idx];
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        it->finish();
    }

    automation_set_enabled(s_prev_auto);    /* 功能：把联动恢复原样 */
    s_active = false;

    ESP_LOGI(TAG, "selftest mode OFF (auto restored to %s)",
             s_prev_auto ? "on" : "off");
    /* 功能：屏幕下一拍自己会刷 */
}

void selftest_run_current(void)
{
    if (!s_active) {
        return;
    }

    const test_item_t *it = &s_items[s_idx];

    /* 功能：重跑前先收个尾 */
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
        it->finish();       /* 功能：切走前先收尾 */
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
        it->finish();       /* 功能：切走前先收尾 */
    }

    s_idx     = (uint8_t)((s_idx + TEST_ITEM_COUNT - 1) % TEST_ITEM_COUNT);
    s_phase   = PHASE_IDLE;
    s_elapsed = 0;

    ESP_LOGI(TAG, "item %d/%d: %s", (int)s_idx + 1, TEST_ITEM_COUNT, s_items[s_idx].name);
}

void selftest_on_adkey(adkey_id_t id, adkey_event_t ev)
{
    if (ev != ADKEY_EVENT_CLICK && ev != ADKEY_EVENT_LONG_PRESS) {
        return;   /* 功能：按下抬起先不管 */
    }

    if (ev == ADKEY_EVENT_LONG_PRESS) {
        if (id == ADKEY_OK) {
            selftest_exit();                   /* 功能：长按收工 */
        }
        return;
    }

    switch (id) {
    case ADKEY_1:
        selftest_exit();                       /* 功能：一号键退出 */
        break;
    case ADKEY_3:
        selftest_prev();                       /* 功能：三号键往上翻 */
        break;
    case ADKEY_4:
        selftest_next();                       /* 功能：四号键往下翻 */
        break;
    case ADKEY_OK:
        selftest_run_current();                /* 功能：确认键就开跑 */
        break;
    default:
        break;                                 /* 功能：二号键先空着 */
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
