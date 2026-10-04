/**
 * @file  adkey.c
 * @brief 五位 AD 键盘驱动实现（IO10 = ADC1_CH9，12dB 衰减）
 *
 * 【实现要点】
 *   1) ADC 读取复用 adc_bus（ADC1 共享单元 + eFuse 曲线校准 → 毫伏值），
 *      不自己建 oneshot 单元 —— 本工程里光敏/雨滴/五键共用同一个 ADC1。
 *   2) 分压档实测（本项目实机 2026-09-28）：
 *        方向键 4 档 ≈ 615 / 1380 / 1968 / 2638 mV，
 *        ★ OK 键 = 【低压档 ≈ 0mV】（模块把 OK 直接对地）——
 *        它【不在】s_key_mv[] 里：那张表只放 4 个方向键，
 *        OK 由 adkey_match() 用 ADKEY_OK_LOW_MAX_MV 单独命中。
 *        （曾漏掉这一档，导致"按 OK 毫无反应"，见 adkey_match 注释）
 *        目前假设键面布局为（十字排列）：
 *                [1]
 *            [4] [OK] [2]
 *                [3]
 *        即 1=上(627)、2=右(2639)、3=下(1971)、4=左(1397)。
 *   3) 消抖 30ms（连续 3 次 10ms 读到同一键才认定），长按 2s，事件语义与 key.c 一致。
 *   4) 扫描任务 10ms 周期，栈 4KB，优先级低于 app_loop。
 */
#include "adkey.h"

#include <string.h>

#include "adc_bus.h"
#include "adkey_logic.h"   /* 纯判定逻辑（电压→键），PC 上单测见 tools/adkey_test.c */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADKEY";

/* ------------------------------------------------------------------ */
/*  参数                                                               */
/* ------------------------------------------------------------------ */

#define ADKEY_SCAN_MS           10      /* 扫描周期 */
#define ADKEY_DEBOUNCE_SAMPLES  3       /* 连续 3 次 = 30ms 消抖 */
#define ADKEY_LONG_PRESS_MS     2000    /* 长按阈值 */
#define ADKEY_IDLE_MIN_MV       3000    /* 自校准有效下限：低于此值说明开机时按着键，拒绝采信 */
#define ADKEY_TASK_STACK_SIZE   4096
#define ADKEY_TASK_PRIORITY     4

/* ★ 关于 OK 键的两次结论修正（阈值宏本身定义在 adkey_logic.h，单测与固件共用）：
 *
 *   旧结论（错的）："OK 只比空闲低 5~27mV（3101 vs 3128），要用窄带 + 开机自校准"。
 *       实际上那是把 ADC 噪声当成了按键信号 —— 用 [3050,3112] 那版固件时，
 *       日志里出现过 "OK DOWN/CLICK/UP" 只隔 30ms 的假事件，人身手速不可能这么快。
 *
 *   新结论（2026-09-28 实机定位，见 docs/11）：
 *       logs/monitor-20260928-205346.log / 205403.log 里四个方向键清清楚楚
 *       （键1=1380 / 键2=2638 / 键3=615 / 键4=1968 mV），而 OK 【一条事件都没有】。
 *       这套模块的五档分压是 0 / 627 / 1397 / 1971 / 2639 mV ——
 *       漏掉的那一档正是【0mV：OK 直接对地】（adkey.h 顶部原本就写着 0(OK 直通 GND)）。
 *
 *   两处把 0mV 吃掉的 bug：
 *     ① adkey_read_mv() 用 `mv > 0` 判有效 → 0mV 被当成"读失败"，替换成空闲值；
 *     ② adkey_match() 只匹配 4 个方向键 + [2900,空闲) 残余档 → 0mV 落不进任何档。
 *   修复：① 判据改 `>= 0`；② 新增 ok_low_max 低压档（见 adkey_logic.h）。 */

/* ------------------------------------------------------------------ */
/*  键值表（同款模块实测，见文件头说明）                                */
/* ------------------------------------------------------------------ */

/* ★ 键位实测（2026-09-28，本项目键盘丝印布局）：
 *        [3]
 *   [1] [OK] [2]
 *        [4]
 *   即：丝印1=左(~1380mV)、丝印2=右(~2638mV)、丝印3=上(~615mV)、丝印4=下(~1968mV)
 *   ★ 丝印 OK = 低压档 ~0mV，【不在这张表里】——由 adkey_logic_match() 的
 *     ok_low_max 命中（曾经漏掉这一档，导致"按 OK 无反应"，见 docs/11）。
 *
 *   判定逻辑本体抽在 adkey_logic.h（纯函数、无 ESP 依赖），
 *   改阈值后可以先用 tools/run_adkey_test.ps1 在电脑上验证一遍再烧板子。 */
static const adkey_logic_cfg_t s_match_cfg = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = ADKEY_OK_HIGH_ENABLE,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

/* ------------------------------------------------------------------ */
/*  状态                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     pressed;         /* 消抖后处于按下状态 */
    bool     long_reported;   /* 本次按下是否已上报过长按 */
    int64_t  press_start_us;
} adkey_ctx_t;

static adkey_ctx_t s_ctx[ADKEY_NUM];
static adkey_id_t  s_stable_key = ADKEY_NUM;   /* 消抖后的"当前按下键"（ADKEY_NUM=无） */
static int         s_idle_mv = 3128;           /* 空闲基线（开机自校准，见 adkey_init） */
static adkey_cb_t  s_cb = NULL;
static void       *s_cb_user = NULL;
static bool        s_inited = false;

/* ------------------------------------------------------------------ */
/*  内部实现                                                           */
/* ------------------------------------------------------------------ */

static void adkey_emit(adkey_id_t id, adkey_event_t ev)
{
    if (s_cb != NULL) {
        /* ⚠ 回调运行在扫描任务上下文：不要阻塞、耗时动作转投队列 */
        s_cb(id, ev, s_cb_user);
    }
}

/** 读毫伏：adc_bus 共享单元 + 校准。
 *  ★ 0mV 是【合法读数】（本键盘 OK 键就是直接对地），不是"读失败"：
 *    adc_bus_read_mv_avg() 只在全部采样失败时返回 -1，所以判据必须是 `>= 0`。
 *    原来写成 `> 0`，把按 OK 时的 0mV 当失败替换成空闲值 → OK 永远按不出来。 */
static int adkey_read_mv(void)
{
    const int mv = adc_bus_read_mv_avg(ADC_CHANNEL_9, 4);
    return (mv >= 0) ? mv : (s_idle_mv + 1);   /* <0 = 真失败 → 当作空闲 */
}

/** 阈值匹配：返回键 id；空闲/落不进任何档 → ADKEY_NUM
 *  判定本体在 adkey_logic.h（纯函数，PC 上可单测）；
 *  这里只多加一条"落不进任何档"的限速告警，方便换键盘时现场标定。 */
static adkey_id_t adkey_match(int mv)
{
    const int r = adkey_logic_match(mv, s_idle_mv, &s_match_cfg);

    /* 非空闲、又落不进任何档：10 秒限速告警一条。
     *
     * ★ 但"松开手之后空闲线自己抖"也会落不进任何档，必须排除，否则日志刷屏：
     *   实测本机空闲基线 3128mV，松开后偶发读到 2905~3124mV（纯 ADC 噪声，
     *   见 adkey_logic.h 里 ADKEY_OK_HIGH_ENABLE 那段记录）。这类读数**高于
     *   最高的方向键档 + 容差**，物理上不可能是方向键，也不可能是 OK 低压档，
     *   所以不是"未标定的按键"，不该告警。
     *   判据取"4 个方向键里最高的那一档 + 容差"，换键盘后自动跟着变。 */
    int top_band = 0;
    for (int i = 0; i < 4; i++) {
        const int hi = (int)s_match_cfg.key_mv[i] + s_match_cfg.tolerance;
        if (hi > top_band) {
            top_band = hi;
        }
    }

    if (r == ADKEY_LOGIC_NONE && mv >= 0 &&
        mv < s_idle_mv - ADKEY_OK_MARGIN_MV && mv < top_band) {
        static int64_t s_warn_us = 0;
        const int64_t now = esp_timer_get_time();
        if (now - s_warn_us > 10000000LL) {
            s_warn_us = now;
            ESP_LOGW(TAG, "读数 %dmV 落不进任何键档（空闲基线 %dmV，最高键档上界 %dmV）——"
                          "若换了键盘，请按一遍各键，把日志里的 mV 填进 s_match_cfg.key_mv[]",
                     mv, s_idle_mv, top_band);
        }
    }
    return (adkey_id_t)r;
}

/* 键名（日志/标定用） */
static const char *const s_key_names[ADKEY_NUM] = { "1", "2", "3", "4", "OK" };

/* 最近一次"认定按下"时的 ADC 读数（诊断用：事件回调里读实时值已经变回空闲了） */
static int s_last_key_mv = -1;

static void adkey_scan_once(void)
{
    const int        mv      = adkey_read_mv();
    const adkey_id_t now_key = adkey_match(mv);
    const int64_t    now_us  = esp_timer_get_time();

    /* ---- 标定辅助：记录"本次按下期间的最低电压" ----
     * 抬起时会打一行 `标定: 键X 最低 NNNmV`。换键盘 / 怀疑键值表时，
     * 按一遍五个键，看这几行就能拿到真实分压值，不用另写测试工具。 */
    static int s_press_min_mv = -1;
    if (s_stable_key != ADKEY_NUM) {
        if (s_press_min_mv < 0 || mv < s_press_min_mv) {
            s_press_min_mv = mv;
        }
    }

    if (now_key == s_stable_key) {
        return;   /* 状态没变（或抖动回到原状态），消抖计数由稳定态直接承担 */
    }

    /* 状态变化：连续确认 3 次才认定（调用方周期 = ADKEY_SCAN_MS） */
    static adkey_id_t pending_key = ADKEY_NUM;
    static int        pending_cnt = 0;

    if (now_key == pending_key) {
        pending_cnt++;
    } else {
        pending_key = now_key;
        pending_cnt = 1;
    }
    /* ★ OK 键用 1 次采样立即锁存：OK 档要么是 0mV（对地，很稳），
     * 要么是"压降只有几 mV"的高压残余档（很浅，3 次消抖会漏）。 */
    const int need = (pending_key == ADKEY_OK) ? 1 : ADKEY_DEBOUNCE_SAMPLES;
    if (pending_cnt < need) {
        return;
    }

    /* ---- 认定稳定变化 ---- */
    const adkey_id_t prev_key = s_stable_key;
    s_stable_key = pending_key;

    if (prev_key != ADKEY_NUM) {
        /* 上一个键抬起 */
        adkey_ctx_t *k = &s_ctx[prev_key];
        const int64_t held_ms = (now_us - k->press_start_us) / 1000;

        k->pressed = false;
        ESP_LOGI(TAG, "标定: 键%s 按下期间最低 %dmV（空闲基线 %dmV，判定线 %dmV）",
                 s_key_names[prev_key], s_press_min_mv, s_idle_mv,
                 s_idle_mv - ADKEY_OK_MARGIN_MV);
        s_press_min_mv = -1;

        if (!k->long_reported && held_ms < ADKEY_LONG_PRESS_MS) {
            adkey_emit(prev_key, ADKEY_EVENT_CLICK);   /* 短按 = 单击 */
        }
        adkey_emit(prev_key, ADKEY_EVENT_UP);
    }

    if (now_key != ADKEY_NUM) {
        /* 新键按下 */
        adkey_ctx_t *k = &s_ctx[now_key];
        k->pressed        = true;
        k->long_reported  = false;
        k->press_start_us = now_us;
        s_last_key_mv     = mv;          /* 让事件回调能打印"真正触发的那次读数" */
        s_press_min_mv    = mv;
        adkey_emit(now_key, ADKEY_EVENT_DOWN);
    }
}

static void adkey_scan_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* ---- 长按判定：按住超过阈值且未上报 → 补一次 LONG_PRESS ---- */
        if (s_stable_key != ADKEY_NUM) {
            adkey_ctx_t *k = &s_ctx[s_stable_key];
            if (k->pressed && !k->long_reported &&
                (esp_timer_get_time() - k->press_start_us) / 1000 >= ADKEY_LONG_PRESS_MS) {
                k->long_reported = true;
                adkey_emit(s_stable_key, ADKEY_EVENT_LONG_PRESS);
            }
        }

        adkey_scan_once();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(ADKEY_SCAN_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  对外接口                                                           */
/* ------------------------------------------------------------------ */

esp_err_t adkey_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    /* ---- 空闲基线自校准：上电采 32 次取平均 ----
     * ADC 通道 CH9（IO10）由 adc_bus_init() 统一配置，这里只依赖它。
     * ★ 判据用 `mv >= 0`：0mV 是合法读数（OK 键对地），只有 <0 才是读失败。 */
    int sum = 0, cnt = 0;
    for (int i = 0; i < 32; i++) {
        const int mv = adc_bus_read_mv_avg(ADC_CHANNEL_9, 4);
        if (mv >= 0) {
            sum += mv;
            cnt++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (cnt > 0) {
        const int avg = sum / cnt;
        if (avg >= ADKEY_IDLE_MIN_MV) {
            s_idle_mv = avg;   /* 正常 ≈3128 */
        } else {
            ESP_LOGW(TAG, "开机基线 %dmV 过低（开机时按着键？），保留默认 %dmV",
                     avg, s_idle_mv);
        }
    }

    if (xTaskCreate(adkey_scan_task, "adkey_scan", ADKEY_TASK_STACK_SIZE, NULL,
                    ADKEY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: IO10(ADC1_CH9), 方向键 %d/%d/%d/%dmV ±%dmV, "
             "OK=低压档[0,%d]mV(高压残余档%s), 空闲基线=%dmV, 消抖 %dms, 长按 %dms",
             ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV,
             ADKEY_TOLERANCE_MV, ADKEY_OK_LOW_MAX_MV,
             ADKEY_OK_HIGH_ENABLE ? "开" : "关",
             s_idle_mv,
             ADKEY_DEBOUNCE_SAMPLES * ADKEY_SCAN_MS, ADKEY_LONG_PRESS_MS);
    return ESP_OK;
}

esp_err_t adkey_register_cb(adkey_cb_t cb, void *user_data)
{
    s_cb      = cb;         /* 只保留最后一个注册者 */
    s_cb_user = user_data;
    return ESP_OK;
}

bool adkey_is_pressed(adkey_id_t id)
{
    return s_stable_key == id;
}

int adkey_raw_mv(void)
{
    return adkey_read_mv();
}

/** 最近一次"认定按下"时的 ADC 读数（mV）；还没按过返回 -1。
 *  事件回调里要打印"真正触发的那次读数"就用它 ——
 *  adkey_raw_mv() 是【实时】读数，等回调跑到时电压早就回到空闲了。 */
int adkey_last_mv(void)
{
    return s_last_key_mv;
}
