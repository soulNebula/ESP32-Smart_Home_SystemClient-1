#include "adkey.h"

#include <string.h>

#include "adc_bus.h"
// 判定规则在这个头里
#include "adkey_logic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADKEY";

// 可调参数
// 多久扫一次键
#define ADKEY_SCAN_MS           10
// 连看三次才算数
#define ADKEY_DEBOUNCE_SAMPLES  3
// 按住多久算长按
#define ADKEY_LONG_PRESS_MS     2000
// 太低就不采信
#define ADKEY_IDLE_MIN_MV       3000
#define ADKEY_TASK_STACK_SIZE   4096
#define ADKEY_TASK_PRIORITY     4

// OK 键是直接接地那档
static const adkey_logic_cfg_t s_match_cfg = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = ADKEY_OK_HIGH_ENABLE,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

// 运行时的状态变量
typedef struct {
    // 抖完还按着吗
    bool     pressed;
    // 长按报过没有
    bool     long_reported;
    int64_t  press_start_us;
} adkey_ctx_t;

static adkey_ctx_t s_ctx[ADKEY_NUM];
// 当前认的是哪个键
static adkey_id_t  s_stable_key = ADKEY_NUM;
// 没按键时的电压
static int         s_idle_mv = 3128;
static adkey_cb_t  s_cb = NULL;
static void       *s_cb_user = NULL;
static bool        s_inited = false;

// 把事件发给外面
static void adkey_emit(adkey_id_t id, adkey_event_t ev)
{
    if (s_cb != NULL) {
        // 这里别做耗时事
        s_cb(id, ev, s_cb_user);
    }
}

// 读一次电压
static int adkey_read_mv(void)
{
    const int mv = adc_bus_read_mv_avg(ADC_CHANNEL_9, 4);
    // 读失败当空闲
    return (mv >= 0) ? mv : (s_idle_mv + 1);
}

// 电压落到哪个键
static adkey_id_t adkey_match(int mv)
{
    const int r = adkey_logic_match(mv, s_idle_mv, &s_match_cfg);

    // 落不进档就限速告警
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

// 键的名字，打日志用
static const char *const s_key_names[ADKEY_NUM] = { "1", "2", "3", "4", "OK" };

// 记下按下那次的电压
static int s_last_key_mv = -1;

// 扫一次键
static void adkey_scan_once(void)
{
    const int        mv      = adkey_read_mv();
    const adkey_id_t now_key = adkey_match(mv);
    const int64_t    now_us  = esp_timer_get_time();

    // 记下按下时的最低值
    static int s_press_min_mv = -1;
    if (s_stable_key != ADKEY_NUM) {
        if (s_press_min_mv < 0 || mv < s_press_min_mv) {
            s_press_min_mv = mv;
        }
    }

    if (now_key == s_stable_key) {
        // 没变就直接返回
        return;
    }

    // 变了要连看几次
    static adkey_id_t pending_key = ADKEY_NUM;
    static int        pending_cnt = 0;

    if (now_key == pending_key) {
        pending_cnt++;
    } else {
        pending_key = now_key;
        pending_cnt = 1;
    }
    // OK 键一次就算数
    const int need = (pending_key == ADKEY_OK) ? 1 : ADKEY_DEBOUNCE_SAMPLES;
    if (pending_cnt < need) {
        return;
    }

    // 认下这次变化
    const adkey_id_t prev_key = s_stable_key;
    s_stable_key = pending_key;

    if (prev_key != ADKEY_NUM) {
        // 上一个键松开了
        adkey_ctx_t *k = &s_ctx[prev_key];
        const int64_t held_ms = (now_us - k->press_start_us) / 1000;

        k->pressed = false;
        ESP_LOGI(TAG, "标定: 键%s 按下期间最低 %dmV（空闲基线 %dmV，判定线 %dmV）",
                 s_key_names[prev_key], s_press_min_mv, s_idle_mv,
                 s_idle_mv - ADKEY_OK_MARGIN_MV);
        s_press_min_mv = -1;

        if (!k->long_reported && held_ms < ADKEY_LONG_PRESS_MS) {
            // 短按算点击
            adkey_emit(prev_key, ADKEY_EVENT_CLICK);
        }
        adkey_emit(prev_key, ADKEY_EVENT_UP);
    }

    if (now_key != ADKEY_NUM) {
        // 新键按下了
        adkey_ctx_t *k = &s_ctx[now_key];
        k->pressed        = true;
        k->long_reported  = false;
        k->press_start_us = now_us;
        // 让回调能打真值
        s_last_key_mv     = mv;
        s_press_min_mv    = mv;
        adkey_emit(now_key, ADKEY_EVENT_DOWN);
    }
}

// 常驻扫描任务
static void adkey_scan_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // 按够久就补发长按
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

// 开机把键盘准备好
esp_err_t adkey_init(void)
{
    if (s_inited) {
        ESP_LOGI(TAG, "already initialized");
        return ESP_OK;
    }

    // 开机先量空闲电压
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
            // 正常就拿它当基线
            s_idle_mv = avg;
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
    // 后登记的顶掉前面
    s_cb      = cb;
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

// 取按下那次的电压
int adkey_last_mv(void)
{
    return s_last_key_mv;
}
