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

// 每十毫秒看一次
#define KEY_SCAN_PERIOD_MS      10
// 栈不能给小
#define KEY_TASK_STACK_SIZE     6144
                                         // 给小了会崩
// 任务的优先级
#define KEY_TASK_PRIORITY       4

// 连读三次才算稳
#define KEY_DEBOUNCE_SAMPLES    (BSP_KEY_DEBOUNCE_MS / KEY_SCAN_PERIOD_MS)

_Static_assert(KEY_DEBOUNCE_SAMPLES >= 1, "BSP_KEY_DEBOUNCE_MS 必须 >= 按键扫描周期");

// 记一个键的状态
typedef struct {
    // 消抖后的电平
    int      stable_level;
    // 刚读到的电平
    int      raw_level;
    // 连着读了几次
    uint32_t same_cnt;
    // 现在按着没有
    bool     pressed;
    // 长按报过没有
    bool     long_reported;
    // 这次按下的时刻
    int64_t  press_start_us;
} key_ctx_t;

// 两个键的引脚
static const gpio_num_t s_key_gpio[BSP_KEY_COUNT] = {
    BSP_KEY_GPIO_KEY1,
    BSP_KEY_GPIO_KEY2,
};

_Static_assert(sizeof(s_key_gpio) / sizeof(s_key_gpio[0]) == BSP_KEY_COUNT,
               "s_key_gpio 表长度必须等于 BSP_KEY_COUNT");

static key_ctx_t s_ctx[BSP_KEY_COUNT];

// 只留最后一个
static key_cb_t s_cb;
static void    *s_cb_user;
// 避免重复初始化
static bool     s_inited;

// 这个电平算按下吗
static inline bool key_level_is_pressed(int level)
{
    return (level == BSP_KEY_ACTIVE_LEVEL);
}

// 把事件报给上层
static void key_emit(key_id_t id, key_event_t ev)
{
    if (s_cb != NULL) {
        s_cb(id, ev, s_cb_user);
    }
}

// 看一遍两个键
static void key_scan_once(void)
{
    const int64_t now_us = esp_timer_get_time();

    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        key_ctx_t  *k  = &s_ctx[i];
        const key_id_t id = (key_id_t)i;
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            // 没接的键不扫
            continue;
        }
        const int   raw = gpio_get_level(s_key_gpio[i]);

        k->raw_level = raw;

        if (raw == k->stable_level) {
            // 没变就把计数清零
            k->same_cnt = 0;
        } else {
            k->same_cnt++;
            if (k->same_cnt < KEY_DEBOUNCE_SAMPLES) {
                // 还没稳再等等
                continue;
            }

            // 读够次数才算真变
            k->same_cnt     = 0;
            k->stable_level = raw;

            if (key_level_is_pressed(raw)) {
                // 稳定按下
                k->pressed        = true;
                k->long_reported  = false;
                k->press_start_us = now_us;
                key_emit(id, KEY_EVENT_DOWN);
            } else {
                // 稳定抬起
                const int64_t held_ms = (now_us - k->press_start_us) / 1000;

                k->pressed = false;

                // 卡在边上补报一次
                if (!k->long_reported && held_ms >= BSP_KEY_LONG_PRESS_MS) {
                    k->long_reported = true;
                    key_emit(id, KEY_EVENT_LONG_PRESS);
                }

                key_emit(id, KEY_EVENT_UP);

                // 短按才算单击
                if (!k->long_reported && held_ms < BSP_KEY_LONG_PRESS_MS) {
                    key_emit(id, KEY_EVENT_CLICK);
                }
                k->long_reported = false;
            }
            continue;
        }

        // 按够了就报长按一次
        if (k->pressed && !k->long_reported &&
            (now_us - k->press_start_us) >= (int64_t)BSP_KEY_LONG_PRESS_MS * 1000) {
            k->long_reported = true;
            key_emit(id, KEY_EVENT_LONG_PRESS);
        }
    }
}

// 反复看按键的任务
static void key_scan_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "scan task start: period=%dms, debounce=%dms(%d samples), long=%dms",
             KEY_SCAN_PERIOD_MS, BSP_KEY_DEBOUNCE_MS,
             (int)KEY_DEBOUNCE_SAMPLES, BSP_KEY_LONG_PRESS_MS);

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        key_scan_once();
        // 按固定周期走不漂
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

// 把按键准备好
esp_err_t key_init(void)
{
    if (s_inited) {
        // 来过就直接返回
        return ESP_OK;
    }

    // 配成输入加上拉
    uint64_t pin_mask = 0;
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            // 弃用的键不配脚
            continue;
        }
        pin_mask |= (1ULL << (uint32_t)s_key_gpio[i]);
    }

    const gpio_config_t io_cfg = {
        .pin_bit_mask = pin_mask,
        .mode         = GPIO_MODE_INPUT,
        // 另头接地要上拉
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        // 只用轮询不用中断
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 先按实际电平当基准
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        if (s_key_gpio[i] == GPIO_NUM_NC) {
            // 弃用的键当没按
            s_ctx[i].stable_level   = 1;
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

    // 把扫键任务起起来
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

// 登记按键回调
esp_err_t key_register_cb(key_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

// 查现在按着没有
bool key_is_pressed(key_id_t id)
{
    if ((int)id < 0 || (int)id >= BSP_KEY_COUNT) {
        return false;
    }

    // 不加锁只读个大概
    return s_ctx[id].pressed;
}
