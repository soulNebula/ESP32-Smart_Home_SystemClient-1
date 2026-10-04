/**
 * @file  key.c
 * @brief 本地按键实现 —— KEY1(BSP_KEY_GPIO_KEY1=GPIO10) / KEY2(BSP_KEY_GPIO_KEY2=GPIO11)
 *        按键一脚接 GPIO、另一脚接 GND，按下为低（BSP_KEY_ACTIVE_LEVEL == 0）。
 *
 * ===========================================================================
 *  实现要点
 * ===========================================================================
 *  1) 不用 GPIO 中断，只用【轮询】：
 *       - 机械按键抖动会产生大量边沿，走中断容易中断风暴，还必须在 ISR 里
 *         回避日志/回调的限制；轮询 + 软件消抖简单可靠，2 个按键毫无压力。
 *       - gpio_config() 里 intr_type = GPIO_INTR_DISABLE。
 *  2) 两个按键共用【一个】扫描任务（10ms 周期，栈 3072，优先级 4），
 *     在一个任务里同时扫 2 个 GPIO，省 RAM 也省调度开销。
 *  3) 消抖：连续 KEY_DEBOUNCE_SAMPLES(= BSP_KEY_DEBOUNCE_MS/10 = 3) 次读到
 *     与"当前稳定电平"相反的电平，才认定电平真的变了（10ms x 3 = 30ms）。
 *  4) 时间基准：用 esp_timer_get_time()（64 位微秒、单调递增、不受 tick 溢出影响）。
 *     不用 xTaskGetTickCount()：它只有 1ms 分辨率，且 32 位 tick 在
 *     CONFIG_FREERTOS_HZ=1000 时约 49.7 天回绕；本模块要测 2000ms 级长按，
 *     用微秒更省心、也不会有回绕比较的坑。
 *  5) 事件顺序（一次完整按下-抬起）：
 *       稳定按下        -> KEY_EVENT_DOWN
 *       按住 >= 2000ms  -> KEY_EVENT_LONG_PRESS（整个按下过程【只报一次】）
 *       稳定抬起        -> KEY_EVENT_UP，且若"未报过长按 且 时长 < 2000ms"
 *                          -> KEY_EVENT_CLICK
 *     即：长按抬起时【不会】再补一个 CLICK。
 *     另外做了边界兜底：若按住时长已经 >= 2000ms 却因为消抖延迟没赶上在按下期间
 *     发长按，会在抬起前补发一次 LONG_PRESS，保证"按够 2 秒必有长按事件"。
 *  6) 上电初始化时用当前实际电平作为初始稳定电平，避免上电瞬间误报 DOWN。
 *
 * ===========================================================================
 *  ⚠ 回调上下文警告
 * ===========================================================================
 *  key_cb_t 回调是在【按键扫描任务】的上下文里被调用的，不是中断。
 *  因此：
 *     · 回调里【不要做阻塞操作】——不要 vTaskDelay / 不要死等信号量或队列 /
 *       不要写 NVS 或 Flash / 不要发大块网络数据（WiFi、MQTT 发布）。
 *     · 回调必须尽快返回。要做耗时动作（连 WiFi、写 Flash、点亮灯带等），
 *       请在回调里 xQueueSend 一条消息给业务任务，由业务任务去做。
 *     · 也不要在回调里调用 key_init()（会造成重入判断的困扰，虽然本文件幂等）。
 */
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "key.h"
#include "board_config.h"

static const char *TAG = "key";

/* -------------------------------------------------------------------------- */
/*  参数                                                                       */
/* -------------------------------------------------------------------------- */
#define KEY_SCAN_PERIOD_MS      10      /* 扫描周期：10ms */
#define KEY_TASK_STACK_SIZE     6144    /* 扫描任务栈（字节）。
                                         * ★ 不能小于 4096：按键回调链会走到
                                         *   device_model → mqtt_publish_state
                                         *   （cJSON 递归 + 事件发布），3072 实测
                                         *   栈溢出 panic（KEY1 单击必崩）。 */
#define KEY_TASK_PRIORITY       4       /* 扫描任务优先级 */

/* 连续读到同一电平多少次才认为稳定：BSP_KEY_DEBOUNCE_MS(30) / 10 = 3 次 */
#define KEY_DEBOUNCE_SAMPLES    (BSP_KEY_DEBOUNCE_MS / KEY_SCAN_PERIOD_MS)

_Static_assert(KEY_DEBOUNCE_SAMPLES >= 1, "BSP_KEY_DEBOUNCE_MS 必须 >= 按键扫描周期");

/* -------------------------------------------------------------------------- */
/*  内部状态                                                                   */
/* -------------------------------------------------------------------------- */
typedef struct {
    int      stable_level;      /* 已消抖的稳定电平 */
    int      raw_level;         /* 最近一次原始读数（调试用） */
    uint32_t same_cnt;          /* 连续读到"与 stable_level 相反"电平的次数 */
    bool     pressed;           /* 稳定状态是否为按下 */
    bool     long_reported;     /* 本次按下是否已经上报过长按 */
    int64_t  press_start_us;    /* 本次按下的起始时刻（esp_timer_get_time()） */
} key_ctx_t;

/* 按键 GPIO 表 —— 引脚号全部来自 board_config.h，本文件不硬编码任何 GPIO 号 */
static const gpio_num_t s_key_gpio[BSP_KEY_COUNT] = {
    BSP_KEY_GPIO_KEY1,
    BSP_KEY_GPIO_KEY2,
};

_Static_assert(sizeof(s_key_gpio) / sizeof(s_key_gpio[0]) == BSP_KEY_COUNT,
               "s_key_gpio 表长度必须等于 BSP_KEY_COUNT");

static key_ctx_t s_ctx[BSP_KEY_COUNT];

static key_cb_t s_cb;           /* 只保留最后一个注册者 */
static void    *s_cb_user;
static bool     s_inited;       /* 幂等标志 */

/* -------------------------------------------------------------------------- */
/*  小工具                                                                     */
/* -------------------------------------------------------------------------- */
static inline bool key_level_is_pressed(int level)
{
    return (level == BSP_KEY_ACTIVE_LEVEL);
}

/**
 * @brief 抛事件给上层
 * @note  ⚠ 这里运行在扫描任务上下文，回调里不要做阻塞操作（详见文件头说明）。
 */
static void key_emit(key_id_t id, key_event_t ev)
{
    if (s_cb != NULL) {
        s_cb(id, ev, s_cb_user);
    }
}

/* -------------------------------------------------------------------------- */
/*  扫描 + 消抖 + 事件判定                                                      */
/* -------------------------------------------------------------------------- */
static void key_scan_once(void)
{
    const int64_t now_us = esp_timer_get_time();

    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        key_ctx_t  *k  = &s_ctx[i];
        const key_id_t id = (key_id_t)i;
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            continue;   /* NC 键不扫描（gpio_get_level(GPIO_NUM_NC) 是非法调用） */
        }
        const int   raw = gpio_get_level(s_key_gpio[i]);

        k->raw_level = raw;

        if (raw == k->stable_level) {
            /* 电平没变（或抖动回到原电平）→ 消抖计数清零 */
            k->same_cnt = 0;
        } else {
            k->same_cnt++;
            if (k->same_cnt < KEY_DEBOUNCE_SAMPLES) {
                continue;   /* 还没稳，继续观察 */
            }

            /* 连续 KEY_DEBOUNCE_SAMPLES 次读到同一新电平 → 认定稳定变化 */
            k->same_cnt     = 0;
            k->stable_level = raw;

            if (key_level_is_pressed(raw)) {
                /* ---- 稳定按下 ---- */
                k->pressed        = true;
                k->long_reported  = false;
                k->press_start_us = now_us;
                key_emit(id, KEY_EVENT_DOWN);
            } else {
                /* ---- 稳定抬起 ---- */
                const int64_t held_ms = (now_us - k->press_start_us) / 1000;

                k->pressed = false;

                /* 边界兜底：消抖要连续 3 次（30ms）才认"抬起"，
                 * 若按住时长刚好卡在阈值附近（例如物理松手在 1995ms、确认抬起在 2025ms），
                 * 长按可能来不及在按下期间发出。这里补发一次，保证
                 * "按够 BSP_KEY_LONG_PRESS_MS 一定有一次 LONG_PRESS 事件"。 */
                if (!k->long_reported && held_ms >= BSP_KEY_LONG_PRESS_MS) {
                    k->long_reported = true;
                    key_emit(id, KEY_EVENT_LONG_PRESS);
                }

                key_emit(id, KEY_EVENT_UP);

                /* 长按已经上报过 → 抬起时不再触发 CLICK；否则短按 = 单击 */
                if (!k->long_reported && held_ms < BSP_KEY_LONG_PRESS_MS) {
                    key_emit(id, KEY_EVENT_CLICK);
                }
                k->long_reported = false;
            }
            continue;
        }

        /* ---- 按下期间：超过长按阈值且还没报过 → 只报一次 LONG_PRESS ---- */
        if (k->pressed && !k->long_reported &&
            (now_us - k->press_start_us) >= (int64_t)BSP_KEY_LONG_PRESS_MS * 1000) {
            k->long_reported = true;
            key_emit(id, KEY_EVENT_LONG_PRESS);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  扫描任务                                                                   */
/* -------------------------------------------------------------------------- */
static void key_scan_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "scan task start: period=%dms, debounce=%dms(%d samples), long=%dms",
             KEY_SCAN_PERIOD_MS, BSP_KEY_DEBOUNCE_MS,
             (int)KEY_DEBOUNCE_SAMPLES, BSP_KEY_LONG_PRESS_MS);

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        key_scan_once();
        /* 固定 10ms 周期，避免采样间隔漂移影响消抖计时 */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */
esp_err_t key_init(void)
{
    if (s_inited) {
        return ESP_OK;              /* 幂等：重复调用直接返回 */
    }

    /* 1. 配置 GPIO：输入 + 内部上拉 + 不用中断 */
    uint64_t pin_mask = 0;
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            continue;   /* NC = 该键弃用（如 KEY1 让位给 AD 键盘），不配置引脚 */
        }
        pin_mask |= (1ULL << (uint32_t)s_key_gpio[i]);
    }

    const gpio_config_t io_cfg = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 按键另一端接 GND → 必须开上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      /* 轮询方案，不用中断 */
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. 用当前实际电平作为初始稳定电平，避免上电瞬间误报 DOWN */
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            s_ctx[i].stable_level   = 1;   /* NC 键固定"未按下"，扫描里也会跳过 */
            s_ctx[i].raw_level      = 1;
            s_ctx[i].pressed        = false;
            continue;
        }
        const int level = gpio_get_level(s_key_gpio[i]);

        s_ctx[i].stable_level   = level;
        s_ctx[i].raw_level      = level;
        s_ctx[i].same_cnt       = 0;
        s_ctx[i].pressed        = key_level_is_pressed(level);
        s_ctx[i].long_reported  = false;
        s_ctx[i].press_start_us = esp_timer_get_time();
    }

    /* 3. 起扫描任务 */
    if (xTaskCreate(key_scan_task, "key_scan", KEY_TASK_STACK_SIZE, NULL,
                    KEY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create key scan task failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: KEY1=GPIO%d, KEY2=GPIO%d, active_level=%d",
             (int)BSP_KEY_GPIO_KEY1, (int)BSP_KEY_GPIO_KEY2, BSP_KEY_ACTIVE_LEVEL);
    return ESP_OK;
}

esp_err_t key_register_cb(key_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

bool key_is_pressed(key_id_t id)
{
    if ((int)id < 0 || (int)id >= BSP_KEY_COUNT) {
        return false;
    }

    /* bool 在 32 位平台上是单字节对齐的原子读写，这里不做加锁，
     * 只保证"读到一个略微过期的稳定状态"，对业务判断无影响。 */
    return s_ctx[id].pressed;
}
