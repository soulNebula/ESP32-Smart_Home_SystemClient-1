#include "input.h"

#include <Arduino.h>

// ======================================================================
// 输入：板载按键 KEY2 + 五位 AD 键盘
//
// 对应 ESP-IDF 工程的 components/BSP/KEY/key.c 和 components/BSP/ADKEY/adkey.c，
// 外加 adkey_logic.h 里那套判键规则。
// 原版两个模块各起一个 10ms 的扫键任务，这边并成一个 input_poll()，
// 主循环反复喊它，里面对着 millis() 走节拍，消抖、单击、长按的判法一个字没改。
// 判键的常量全部对着 board_config.h 里的 BSP_ADKEY_* 走，跟键位表分流点是一份数。
// ======================================================================

// 原版两个扫键任务都是每十毫秒看一次，这边合成一个节拍
#define APP_INPUT_SCAN_MS           10

// 连读三次才算稳，跟原版的 KEY_DEBOUNCE_SAMPLES / ADKEY_DEBOUNCE_SAMPLES 一个数
#define APP_KEY_DEBOUNCE_SAMPLES    (BSP_KEY_DEBOUNCE_MS / APP_INPUT_SCAN_MS)
#define APP_ADKEY_DEBOUNCE_SAMPLES  (BSP_ADKEY_DEBOUNCE_MS / APP_INPUT_SCAN_MS)

// 周期不能比消抖还长，不然连读次数会变成零
static_assert(APP_KEY_DEBOUNCE_SAMPLES >= 1, "BSP_KEY_DEBOUNCE_MS 必须 >= 按键扫描周期");
static_assert(APP_ADKEY_DEBOUNCE_SAMPLES >= 1, "BSP_ADKEY_DEBOUNCE_MS 必须 >= 键盘扫描周期");

// 键盘那路的参数，板级配置里没有的照 adkey_logic.h 补
// 贴太近就当空闲，免得噪声被当成 OK
#define APP_ADKEY_OK_MARGIN_MV      3
// 贴近满压也算 OK 的那档默认关掉，开了会误触
#define APP_ADKEY_OK_HIGH_ENABLE    0
// 高压档的门槛，档关着也留个数
#define APP_ADKEY_OK_MIN_MV         2900
// 那一路连采几次取平均
#define APP_ADKEY_ADC_SAMPLES       4
// 落不进任何键档的告警限速，十秒最多一句
#define APP_ADKEY_WARN_MS           10000

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
    uint32_t press_start_ms;
} key_ctx_t;

// 键盘上一个键的状态
typedef struct {
    // 抖完还按着吗
    bool     pressed;
    // 长按报过没有
    bool     long_reported;
    // 这次按下的时刻
    uint32_t press_start_ms;
} adkey_ctx_t;

// 两个按键的脚，KEY1 那个脚让给键盘了，填 -1 就当没接
static const int s_key_gpio[KEY_ID_MAX] = {
    BSP_KEY_GPIO_KEY1,
    BSP_KEY_GPIO_KEY2,
};

static key_ctx_t s_key_ctx[KEY_ID_MAX];

// 键盘运行时的状态
static adkey_ctx_t s_adkey_ctx[ADKEY_NUM];
// 当前认的是哪个键
static adkey_id_t  s_stable_key = ADKEY_NUM;
// 没按键时的电压
static int         s_idle_mv = 3128;
// 变了的键要连看几次才认
static adkey_id_t  s_pending_key = ADKEY_NUM;
static int         s_pending_cnt = 0;
// 记下按下那次的电压
static int         s_last_key_mv = -1;
// 按下期间的最低电压，松开时打出来标定用
static int         s_press_min_mv = -1;
// 上一次告警的时刻
static uint32_t    s_warn_ms = 0;

// 只留最后一个
static key_cb_t    s_key_cb;
static void       *s_key_cb_user;
static adkey_cb_t  s_adkey_cb;
static void       *s_adkey_cb_user;

// 上次扫键的时刻
static uint32_t    s_last_scan_ms;
static bool        s_inited;

// 键的名字，打日志用
static const char *const s_key_names[ADKEY_NUM] = { "1", "2", "3", "4", "OK" };

// 读一次电压
static int adc_read_mv_avg(int gpio, int samples) {
    int32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        // 这个口自带校准，衰减用默认那档，跟原版的 ADC_ATTEN_DB_12 是一回事
        sum += (int32_t)analogReadMilliVolts(gpio);
    }
    // 四舍五入
    return (int)((sum + samples / 2) / samples);
}

// 读一次键盘电压
static int adkey_read_mv(void) {
    // 原版连采四次取平均，这边照旧
    // 原版读失败会返回负数、当空闲处理，analogReadMilliVolts 不会失败，那条分支就没搬
    return adc_read_mv_avg(BSP_ADKEY_GPIO, APP_ADKEY_ADC_SAMPLES);
}

// 电压落到哪个键，规则照 adkey_logic.h 抄
static adkey_id_t adkey_match(int mv) {
    // 跟空闲挨着算没按
    if (mv < 0 || mv >= s_idle_mv - APP_ADKEY_OK_MARGIN_MV) {
        return ADKEY_NUM;
    }

    // 最低档就是 OK，直接对地那档
    if (mv <= BSP_ADKEY_OK_LOW_MAX_MV) {
        return ADKEY_OK;
    }

    // 贴近满压也算 OK
    if (APP_ADKEY_OK_HIGH_ENABLE && mv >= APP_ADKEY_OK_MIN_MV) {
        return ADKEY_OK;
    }

    // 挑离得最近的那个键
    static const int s_key_mv[4] = {
        BSP_ADKEY_KEY1_MV,
        BSP_ADKEY_KEY2_MV,
        BSP_ADKEY_KEY3_MV,
        BSP_ADKEY_KEY4_MV,
    };
    int best     = ADKEY_NUM;
    int min_diff = BSP_ADKEY_TOLERANCE_MV;
    for (int i = 0; i < 4; i++) {
        int diff = mv - s_key_mv[i];
        if (diff < 0) {
            diff = -diff;
        }
        if (diff <= min_diff) {
            min_diff = diff;
            best     = i;
        }
    }

    // 落不进档就限速告警
    int top_band = 0;
    for (int i = 0; i < 4; i++) {
        const int hi = s_key_mv[i] + BSP_ADKEY_TOLERANCE_MV;
        if (hi > top_band) {
            top_band = hi;
        }
    }
    if (best == ADKEY_NUM && mv < s_idle_mv - APP_ADKEY_OK_MARGIN_MV && mv < top_band) {
        if ((uint32_t)(millis() - s_warn_ms) > APP_ADKEY_WARN_MS) {
            s_warn_ms = millis();
            Serial.printf("ADKEY: 读数 %dmV 落不进任何键档（空闲基线 %dmV，最高键档上界 %dmV）——"
                          "若换了键盘，请按一遍各键，把日志里的 mV 填进 board_config.h 的 BSP_ADKEY_KEYx_MV\n",
                          mv, s_idle_mv, top_band);
        }
    }

    // 落不进档就算没按
    return (adkey_id_t)best;
}

// 这个电平算按下吗
static inline bool key_level_is_pressed(int level) {
    return (level == BSP_KEY_ACTIVE_LEVEL);
}

// 把按键事件报给上层
static void key_emit(key_id_t id, key_event_t ev) {
    if (s_key_cb != NULL) {
        s_key_cb(id, ev, s_key_cb_user);
    }
}

// 把键盘事件报给上层
static void adkey_emit(adkey_id_t id, adkey_event_t ev) {
    if (s_adkey_cb != NULL) {
        // 这里别做耗时事
        s_adkey_cb(id, ev, s_adkey_cb_user);
    }
}

// 看一遍按键
static void key_scan_once(void) {
    const uint32_t now_ms = millis();

    for (int i = 0; i < KEY_ID_MAX; i++) {
        key_ctx_t     *k  = &s_key_ctx[i];
        const key_id_t id = (key_id_t)i;
        if (s_key_gpio[i] < 0) {
            // 没接的键不扫
            continue;
        }
        const int raw = digitalRead(s_key_gpio[i]);

        k->raw_level = raw;

        if (raw == k->stable_level) {
            // 没变就把计数清零
            k->same_cnt = 0;
        } else {
            k->same_cnt++;
            if (k->same_cnt < APP_KEY_DEBOUNCE_SAMPLES) {
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
                k->press_start_ms = now_ms;
                key_emit(id, KEY_EVENT_DOWN);
            } else {
                // 稳定抬起
                const uint32_t held_ms = now_ms - k->press_start_ms;

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
            (now_ms - k->press_start_ms) >= BSP_KEY_LONG_PRESS_MS) {
            k->long_reported = true;
            key_emit(id, KEY_EVENT_LONG_PRESS);
        }
    }
}

// 按够久就补发长按
static void adkey_check_long(void) {
    if (s_stable_key == ADKEY_NUM) {
        return;
    }

    adkey_ctx_t *k = &s_adkey_ctx[s_stable_key];
    if (k->pressed && !k->long_reported &&
        (millis() - k->press_start_ms) >= BSP_ADKEY_LONG_PRESS_MS) {
        k->long_reported = true;
        adkey_emit(s_stable_key, ADKEY_EVENT_LONG_PRESS);
    }
}

// 扫一次键盘
static void adkey_scan_once(void) {
    const int        mv      = adkey_read_mv();
    const adkey_id_t now_key = adkey_match(mv);
    const uint32_t   now_ms  = millis();

    // 记下按下时的最低值
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
    if (now_key == s_pending_key) {
        s_pending_cnt++;
    } else {
        s_pending_key = now_key;
        s_pending_cnt = 1;
    }
    // OK 键一次就算数
    const int need = (s_pending_key == ADKEY_OK) ? 1 : APP_ADKEY_DEBOUNCE_SAMPLES;
    if (s_pending_cnt < need) {
        return;
    }

    // 认下这次变化
    const adkey_id_t prev_key = s_stable_key;
    s_stable_key = s_pending_key;

    if (prev_key != ADKEY_NUM) {
        // 上一个键松开了
        adkey_ctx_t   *k       = &s_adkey_ctx[prev_key];
        const uint32_t held_ms = now_ms - k->press_start_ms;

        k->pressed = false;
        Serial.printf("ADKEY: 标定: 键%s 按下期间最低 %dmV（空闲基线 %dmV，判定线 %dmV）\n",
                      s_key_names[prev_key], s_press_min_mv, s_idle_mv,
                      s_idle_mv - APP_ADKEY_OK_MARGIN_MV);
        s_press_min_mv = -1;

        if (!k->long_reported && held_ms < BSP_ADKEY_LONG_PRESS_MS) {
            // 短按算点击
            adkey_emit(prev_key, ADKEY_EVENT_CLICK);
        }
        adkey_emit(prev_key, ADKEY_EVENT_UP);
    }

    if (now_key != ADKEY_NUM) {
        // 新键按下了
        adkey_ctx_t *k = &s_adkey_ctx[now_key];
        k->pressed        = true;
        k->long_reported  = false;
        k->press_start_ms = now_ms;
        // 让回调能打真值
        s_last_key_mv     = mv;
        s_press_min_mv    = mv;
        adkey_emit(now_key, ADKEY_EVENT_DOWN);
    }
}

// 把按键和键盘准备好
bool input_init(void) {
    if (s_inited) {
        // 来过就直接返回
        return true;
    }

    // KEY2 另头接地，按下是低，靠内部上拉
    if (BSP_KEY_GPIO_KEY2 >= 0) {
        pinMode(BSP_KEY_GPIO_KEY2, INPUT_PULLUP);
    }

    // 键盘那路只量电压，千万别给这脚上拉：键盘靠分压出电压，一上拉分压点全变了
    pinMode(BSP_ADKEY_GPIO, INPUT);

    // 先按实际电平当基准
    for (int i = 0; i < KEY_ID_MAX; i++) {
        key_ctx_t *k = &s_key_ctx[i];
        if (s_key_gpio[i] < 0) {
            // 弃用的键当没按
            k->stable_level = 1;
            k->raw_level    = 1;
            k->pressed      = false;
            continue;
        }
        const int level = digitalRead(s_key_gpio[i]);

        k->stable_level   = level;
        k->raw_level      = level;
        k->same_cnt       = 0;
        k->pressed        = key_level_is_pressed(level);
        k->long_reported  = false;
        k->press_start_ms = millis();
    }

    // 开机先量空闲电压，连采 32 次取平均
    // 原版隔 5ms 采一次凑够 160ms，这边一口气采完就够稳，省得开机白站一下
    int32_t sum = 0;
    for (int i = 0; i < 32; i++) {
        sum += adkey_read_mv();
    }
    const int avg = (int)(sum / 32);
    if (avg >= BSP_ADKEY_IDLE_MIN_MV) {
        // 正常就拿它当基线
        s_idle_mv = avg;
    } else {
        Serial.printf("ADKEY: 开机基线 %dmV 过低（开机时按着键？），保留默认 %dmV\n",
                      avg, s_idle_mv);
    }

    s_last_scan_ms = millis();
    s_inited       = true;

    Serial.printf("KEY: init ok: KEY1=GPIO%d(没接), KEY2=GPIO%d, active_level=%d\n",
                  (int)BSP_KEY_GPIO_KEY1, (int)BSP_KEY_GPIO_KEY2, BSP_KEY_ACTIVE_LEVEL);
    Serial.printf("ADKEY: init ok: GPIO%d, 方向键 %d/%d/%d/%dmV ±%dmV, "
                  "OK=低压档[0,%d]mV(高压残余档%s), 空闲基线=%dmV, 消抖 %dms, 长按 %dms\n",
                  BSP_ADKEY_GPIO,
                  BSP_ADKEY_KEY1_MV, BSP_ADKEY_KEY2_MV, BSP_ADKEY_KEY3_MV, BSP_ADKEY_KEY4_MV,
                  BSP_ADKEY_TOLERANCE_MV, BSP_ADKEY_OK_LOW_MAX_MV,
                  APP_ADKEY_OK_HIGH_ENABLE ? "开" : "关",
                  s_idle_mv,
                  APP_ADKEY_DEBOUNCE_SAMPLES * APP_INPUT_SCAN_MS, BSP_ADKEY_LONG_PRESS_MS);
    return true;
}

// 登记回调，只留最后一个
void key_register_cb(key_cb_t cb, void *user_data) {
    s_key_cb      = cb;
    s_key_cb_user = user_data;
}

// 键盘的回调，后登记的顶掉前面
void adkey_register_cb(adkey_cb_t cb, void *user_data) {
    s_adkey_cb      = cb;
    s_adkey_cb_user = user_data;
}

// 主循环喊这个，消抖和长短按都在里面算
void input_poll(void) {
    const uint32_t now_ms = millis();

    // 一个节拍里两路各扫一次，没到点就直接回去
    // 时刻记成"现在"而不是累加：主循环慢了也只是少扫几次，不会把消抖次数凑快
    if ((uint32_t)(now_ms - s_last_scan_ms) < APP_INPUT_SCAN_MS) {
        return;
    }
    s_last_scan_ms = now_ms;

    // 按键那一路
    key_scan_once();

    // 键盘那一路：先补长按，再看有没有新变化
    adkey_check_long();
    adkey_scan_once();
}

// 这个键按着没
bool key_is_pressed(key_id_t id) {
    if ((int)id < 0 || (int)id >= KEY_ID_MAX) {
        return false;
    }

    // 不加锁只读个大概
    return s_key_ctx[id].pressed;
}

// 这个键盘键按着没
bool adkey_is_pressed(adkey_id_t id) {
    return s_stable_key == id;
}

// 读现在多少伏
int adkey_raw_mv(void) {
    return adkey_read_mv();
}

// 读按下那刻的电压
int adkey_last_mv(void) {
    return s_last_key_mv;
}
