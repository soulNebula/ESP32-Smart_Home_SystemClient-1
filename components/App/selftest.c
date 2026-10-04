/**
 * @file  selftest.c
 * @brief 按键自检模式实现（OLED 显示 + 五位键盘驱动，见 selftest.h 顶部说明）
 *
 * 【实现要点】
 *   1) 不自建任务：状态机由 app_loop 每 500ms 调 selftest_tick() 推进，
 *      OLED 刷新也走同一节拍（与仪表盘一致）。
 *   2) 测试动作全部走 device_model 的 set 接口（SRC_SELFTEST），
 *      和正常业务链路同一条代码路径，测出来的就是真实功能。
 *   3) 舵机项：转到 90° 停留 1.5s 后回 0°（关闭位），随后由 servo 驱动的
 *      "到位自动松劲"自然释放，不需要额外处理。
 *   4) OLED 文案只用 wqy12 的 GB2312 字集里的字（生僻字会变方块）。
 *   5) 键位：3=上一项 4=下一项 OK单击=执行 OK长按=退出。
 */
#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "device_model.h"
#include "esp_log.h"
/* OLED 渲染已移交 astra UI（u8g2 画布 + oled_write_page），本模块只管状态机 */
#include "sensor.h"

static const char *TAG = "SELFTEST";

/* ------------------------------------------------------------------ */
/*  测试项定义                                                         */
/* ------------------------------------------------------------------ */

#define TEST_TICK_MS        500     /* app_loop 的 tick 周期 */

typedef struct {
    const char *name;               /* 中文名（字库内的字） */
    void      (*start)(void);       /* 测试开始动作 */
    void      (*finish)(void);      /* 测试收尾动作（把设备恢复） */
    uint8_t    run_ticks;           /* 执行阶段持续多少 tick（0 = 无动作立即完成） */
} test_item_t;

/* 各测试项的动作：全部走 device_model，与正常业务同路径 */
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
static void t_window(void)      { device_set_level(DEV_WINDOW, 50, SRC_SELFTEST); }   /* 50% = 90° */
static void t_window_end(void)  { device_set_level(DEV_WINDOW, 0, SRC_SELFTEST); }
static void t_door(void)        { device_set_level(DEV_DOOR, 50, SRC_SELFTEST); }
static void t_door_end(void)    { device_set_level(DEV_DOOR, 0, SRC_SELFTEST); }
static void t_curtain(void)     { device_set_level(DEV_CURTAIN, 50, SRC_SELFTEST); }
static void t_curtain_end(void) { device_set_level(DEV_CURTAIN, 0, SRC_SELFTEST); }

#define TEST_ITEM_COUNT         9
#define TEST_IDX_SENSOR         (TEST_ITEM_COUNT - 1)   /* 最后一项 = 传感器读数 */

static const test_item_t s_items[TEST_ITEM_COUNT] = {
    { "客厅灯", t_led_living,  t_led_living_end,  4 },   /* 亮 2s */
    { "厨房灯", t_led_kitchen, t_led_kitchen_end, 4 },
    { "卧室灯", t_led_bedroom, t_led_bedroom_end, 4 },
    { "浴室灯", t_led_bath,    t_led_bath_end,    4 },
    { "风扇",   t_fan,         t_fan_end,         4 },   /* 60% 2s（硬件没装只验软件路径） */
    { "窗户",   t_window,      t_window_end,      3 },   /* 90° 停 1.5s 回 0° */
    { "门",     t_door,        t_door_end,        3 },
    { "窗帘",   t_curtain,     t_curtain_end,     3 },
    { "传感器", NULL,          NULL,              0 },   /* 只显示读数 */
};

/* ------------------------------------------------------------------ */
/*  状态                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    PHASE_IDLE = 0,   /* 等待：显示"等待"，按 KEY1 执行 */
    PHASE_RUN,        /* 执行中：显示"检测中" */
    PHASE_DONE,       /* 完成：显示"完成" */
} phase_t;

static bool     s_active    = false;
static bool     s_prev_auto = true;     /* 进入自检前的自动联动开关 */
static uint8_t  s_idx       = 0;
static phase_t  s_phase     = PHASE_IDLE;
static uint16_t s_elapsed   = 0;        /* RUN 阶段已过的 tick 数 */

/* ------------------------------------------------------------------ */
/*  OLED 渲染                                                          */
/* ------------------------------------------------------------------ */

/** 传感器页：两行 16x16 中文 + 读数（温度/湿度一行，光照/雨滴一行） */
/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

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
    automation_set_enabled(false);          /* 自检期间暂停联动，避免规则干扰测试 */
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

    /* 执行中的项先收尾（灯关掉/舵机回 0°），避免带着"半成品"退出 */
    const test_item_t *it = &s_items[s_idx];
    if (s_phase == PHASE_RUN && it->finish != NULL) {
        it->finish();
    }

    automation_set_enabled(s_prev_auto);    /* 恢复进入前的自动联动状态（不写 NVS） */
    s_active = false;

    ESP_LOGI(TAG, "selftest mode OFF (auto restored to %s)",
             s_prev_auto ? "on" : "off");
    /* 仪表盘由 app_loop 下一拍刷新，这里不用管 OLED */
}

void selftest_run_current(void)
{
    if (!s_active) {
        return;
    }

    const test_item_t *it = &s_items[s_idx];

    /* 重复执行：先把上一轮的收尾动作补上，再从干净状态开始 */
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
        it->finish();       /* 切走前收尾当前项 */
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
        it->finish();       /* 切走前收尾当前项 */
    }

    s_idx     = (uint8_t)((s_idx + TEST_ITEM_COUNT - 1) % TEST_ITEM_COUNT);
    s_phase   = PHASE_IDLE;
    s_elapsed = 0;

    ESP_LOGI(TAG, "item %d/%d: %s", (int)s_idx + 1, TEST_ITEM_COUNT, s_items[s_idx].name);
}

void selftest_on_adkey(adkey_id_t id, adkey_event_t ev)
{
    if (ev != ADKEY_EVENT_CLICK && ev != ADKEY_EVENT_LONG_PRESS) {
        return;   /* DOWN/UP 不处理 */
    }

    if (ev == ADKEY_EVENT_LONG_PRESS) {
        if (id == ADKEY_OK) {
            selftest_exit();                   /* OK 长按退出 */
        }
        return;
    }

    switch (id) {
    case ADKEY_1:
        selftest_exit();                       /* 1 = 退出（OK 键长按不可靠，见 main.c 注释） */
        break;
    case ADKEY_3:
        selftest_prev();                       /* 3 = 上一项 */
        break;
    case ADKEY_4:
        selftest_next();                       /* 4 = 下一项 */
        break;
    case ADKEY_OK:
        selftest_run_current();                /* OK 单击 = 执行 */
        break;
    default:
        break;                                 /* 2 暂未分配功能 */
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

/* ------------------------------------------------------------------ */
/*  状态查询（astra UI 每帧刷新用）                                    */
/* ------------------------------------------------------------------ */

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
