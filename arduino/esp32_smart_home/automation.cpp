#include "automation.h"

#include <Arduino.h>
#include <Preferences.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ======================================================================
// 自动联动：光照、温度、下雨三条线
//
// 对应 ESP-IDF 工程的 components/App/automation.c。
// 出厂默认值、范围检查、三条规则的判定、日志内容都照那份抄，数值一个没改。
//
// 存的地方：原版用 NVS 整包，这版用 Preferences 存同一套结构 ——
// 一样是 magic + version + cfg_size 加一条 automation_cfg_t 的二进制整包，
// 命名空间和 key 都没改（sh_auto / cfg），读回来先对记号、版本、长度，
// 对不上就当没存过、退回出厂默认，两边行为一致，以后也好对着看。
//
// 换成 Arduino 之后有几处不一样，下面都注了：
//   1. 原版用互斥锁护着配置，因为网络、蓝牙、主任务会同时进来改；
//      这版所有改动都从主循环进（网络和蓝牙的回调只把报文排队），
//      所以不加锁也不会打架；
//   2. 时间用 millis()，原版的 esp_timer_get_time()/1000 也是毫秒；
//      手动保护的时间戳用 uint32_t 存、相减比较，跨回绕也不会算错；
//   3. 原版主循环 500ms 转一圈跑一次规则，这边主循环快得多，
//      tick 自己按同一节拍节流，不 delay 也不阻塞；
//   4. 这块板子没有 BH1750，光照只有光敏电阻那路，lux 照原版光敏那路的
//      算法从百分比折成等效值，再跟 light_on_lux / light_off_lux 比。
// ======================================================================

static const char *const TAG = "automation";

// 存东西的地方，跟原版一个名字
#define AUTO_NVS_NAMESPACE  "sh_auto"
#define AUTO_NVS_KEY        "cfg"

// 四个字母拼成记号
#define AUTO_CFG_MAGIC      ((uint32_t)('A') | ((uint32_t)('U') << 8) | \
                             ((uint32_t)('T') << 16) | ((uint32_t)('O') << 24))
#define AUTO_CFG_VERSION    1

// 规则多久看一遍：原版主循环一圈就是 500ms，这边主循环快得多，自己按这个节拍节流
#define AUTO_TICK_MS        500u

// 光敏百分比折等效 lux 的系数，跟原版 LIGHT_EQ_LUX_GAIN 一样，全亮 100% 折成 500
#define AUTO_LIGHT_EQ_LUX_GAIN  5.0f

// 出厂默认开不开自动：原版看 CONFIG_APP_AUTO_ENABLE_DEFAULT，本工程那份 sdkconfig
// 是开着的（build/config/sdkconfig.h 里有），这边也照着默认开
#define AUTO_DEF_ENABLED    true

// 存到 flash 里的整包，排法跟原版一模一样
typedef struct {
    // 记号，认是不是自己的
    uint32_t magic;
    // 第几版，好认旧数据
    uint16_t version;
    // 配置多大，对一下
    uint16_t cfg_size;
    // 真正那几条线
    automation_cfg_t cfg;
} auto_cfg_blob_t;

// 头固定八字节
static_assert(offsetof(auto_cfg_blob_t, cfg) == 8,
              "auto_cfg_blob_t header must stay exactly 8 bytes");
// 配置别太大，装得下
static_assert(sizeof(automation_cfg_t) < 65535, "automation_cfg_t too large for the blob header");

// 出厂默认那几条线，值全取自 board_config.h 的 BSP_DEF_*
// 原版这儿是个 AUTO_CFG_DEFAULT_INIT 宏，C++ 里写成函数一样，值一个没改
static automation_cfg_t cfg_defaults(void) {
    automation_cfg_t c;

    memset(&c, 0, sizeof(c));

    c.enabled            = AUTO_DEF_ENABLED;
    // 暗过五十就开灯
    c.light_on_lux       = (float)BSP_DEF_LIGHT_ON_LUX;
    // 亮过两百就关灯
    c.light_off_lux      = (float)BSP_DEF_LIGHT_OFF_LUX;
    // 热过二十八开风扇
    c.temp_fan_on_c      = (float)BSP_DEF_TEMP_FAN_ON_C;
    // 凉到二十六关风扇
    c.temp_fan_off_c     = (float)BSP_DEF_TEMP_FAN_OFF_C;
    // 自动风速七成
    c.fan_auto_speed     = 70;
    // 湿过三十算下雨
    c.rain_pct           = (float)BSP_DEF_RAIN_PCT;
    // 雨停自动开窗
    c.auto_window_reopen = true;
    c.auto_light_enable  = true;
    c.auto_temp_enable   = true;
    c.auto_rain_enable   = true;

    return c;
}

// 眼下用的那几条线
static automation_cfg_t s_cfg = cfg_defaults();

// 记下每台人动过的时间，0 表示没人动过
static uint32_t s_last_manual_ms[DEV_COUNT];

// 存东西的地方只开一次，别反复 begin/end
static Preferences s_prefs;
static bool        s_prefs_open = false;

// 上次跑规则的时刻，用来节流
static uint32_t s_last_tick_ms = 0u;

static bool s_inited = false;

// 一次写好默认那几条线
static void cfg_set_defaults(automation_cfg_t *c) {
    *c = cfg_defaults();
}

// 换一整份配置。改动只在主循环里进来，不加锁
static void cfg_store(const automation_cfg_t *c) {
    s_cfg = *c;
}

// 抄一份配置出来
static void cfg_snapshot(automation_cfg_t *out) {
    *out = s_cfg;
}

// 查查这几条线合不合理
static bool cfg_validate(const automation_cfg_t *c) {
    if (!isfinite(c->light_on_lux) || !isfinite(c->light_off_lux) ||
        !isfinite(c->temp_fan_on_c) || !isfinite(c->temp_fan_off_c) ||
        !isfinite(c->rain_pct)) {
        return false;
    }
    if (c->light_on_lux <= 0.0f) {
        // 开灯线得是正数
        return false;
    }
    if (c->light_off_lux <= c->light_on_lux) {
        // 两条线高低写反了
        return false;
    }
    if ((c->temp_fan_on_c < -10.0f) || (c->temp_fan_on_c > 60.0f)) {
        return false;
    }
    if (c->temp_fan_off_c >= c->temp_fan_on_c) {
        // 关风扇的线得低些
        return false;
    }
    if ((c->rain_pct < 0.0f) || (c->rain_pct > 100.0f)) {
        return false;
    }
    if (c->fan_auto_speed > 100) {
        return false;
    }
    return true;
}

// 光照这路的数是不是真的
static bool light_data_valid(const sensor_data_t *d) {
    // 板子上没 BH1750，只看光敏电阻那两路有没有读数，比原版少了那个标记位
    return (d->light_mv > 0) || (d->light_pct > 0.0f);
}

// 光照百分比折成等效 lux，跟原版光敏电阻那路的算法一样：先平方再乘系数
static float light_eq_lux(const sensor_data_t *d) {
    return d->light_pct * d->light_pct / 100.0f * AUTO_LIGHT_EQ_LUX_GAIN;
}

// 看这台现在让不让碰
static bool guard_ok(device_id_t id, uint32_t now_ms) {
    if (((int)id < 0) || (id >= DEV_COUNT)) {
        return true;
    }

    const uint32_t last = s_last_manual_ms[id];
    if (last == 0u) {
        // 没人动过，随便调
        return true;
    }
    // 两数相减比，够久就放行；这么写跨 millis() 回绕也不会算错
    return (uint32_t)(now_ms - last) >= (uint32_t)AUTO_MANUAL_GUARD_MS;
}

// 上电把存的那几条线读回来
bool automation_init(void) {
    if (s_inited) {
        Serial.printf("%s: 已经初始化过\n", TAG);
        return true;
    }

    // 存东西的地方先备好；Preferences 得等 setup() 之后才能开，这儿只开一次
    if (!s_prefs_open) {
        s_prefs_open = s_prefs.begin(AUTO_NVS_NAMESPACE, false);
        if (!s_prefs_open) {
            // 开不了就没法存也没法读，配置照默认值跑，不算致命
            Serial.printf("%s: prefs.begin(%s) 没成，自动配置只能用默认值\n",
                          TAG, AUTO_NVS_NAMESPACE);
        }
    }

    // 读不到就用默认值
    const bool loaded = automation_load();
    s_inited = true;

    // 头一回 tick 别干等半个节拍，读回配置就马上跑
    s_last_tick_ms = (uint32_t)(millis() - AUTO_TICK_MS);

    Serial.printf("%s: 初始化完成（%s）\n", TAG,
                  loaded ? "配置从 NVS 读回" : "用出厂默认值");

    // 留够地方拼 JSON
    char buf[320];
    if (automation_cfg_json(buf, sizeof(buf)) > 0) {
        Serial.printf("%s: cfg = %s\n", TAG, buf);
    }

    // 配置坏了也别起不来
    return true;
}

// 从存的地方读回来
bool automation_load(void) {
    automation_cfg_t loaded;

    cfg_set_defaults(&loaded);

    if (!s_prefs_open) {
        // 存东西的地方没开，只能默认
        Serial.printf("%s: 存储没开，用出厂默认值\n", TAG);
        cfg_store(&loaded);
        return false;
    }

    auto_cfg_blob_t blob;

    memset(&blob, 0, sizeof(blob));

    // 先问有多长，跟"没存过"分开看
    const size_t stored = s_prefs.getBytesLength(AUTO_NVS_KEY);
    if (stored == 0) {
        // 头回上电没存过，正常，安静地用默认值就行，别报错
        Serial.printf("%s: 没找到存档（头回上电？），用出厂默认值\n", TAG);
        cfg_store(&loaded);
        return false;
    }
    if (stored != sizeof(blob)) {
        Serial.printf("%s: 存档 %u 字节，不是 %u，丢掉用默认值\n",
                      TAG, (unsigned)stored, (unsigned)sizeof(blob));
        cfg_store(&loaded);
        return false;
    }

    const size_t got = s_prefs.getBytes(AUTO_NVS_KEY, &blob, sizeof(blob));
    if (got != sizeof(blob)) {
        Serial.printf("%s: 存档读回来 %u 字节，不是 %u，丢掉用默认值\n",
                      TAG, (unsigned)got, (unsigned)sizeof(blob));
        cfg_store(&loaded);
        return false;
    }
    if ((blob.magic != AUTO_CFG_MAGIC) || (blob.version != AUTO_CFG_VERSION)) {
        Serial.printf("%s: 存档记号/版本对不上（0x%08x v%u），丢掉用默认值\n",
                      TAG, (unsigned)blob.magic, (unsigned)blob.version);
        cfg_store(&loaded);
        return false;
    }
    if ((blob.cfg_size != sizeof(automation_cfg_t)) || !cfg_validate(&blob.cfg)) {
        Serial.printf("%s: 存档内容不对（%u 字节），丢掉用默认值\n",
                      TAG, (unsigned)blob.cfg_size);
        cfg_store(&loaded);
        return false;
    }

    cfg_store(&blob.cfg);
    Serial.printf("%s: 配置从 NVS 读回\n", TAG);
    return true;
}

// 存起来，掉电不丢
bool automation_save(void) {
    auto_cfg_blob_t blob;

    memset(&blob, 0, sizeof(blob));

    blob.magic    = AUTO_CFG_MAGIC;
    blob.version  = AUTO_CFG_VERSION;
    blob.cfg_size = (uint16_t)sizeof(automation_cfg_t);
    cfg_snapshot(&blob.cfg);

    if (!s_prefs_open) {
        // 存不下就算了，当下这份照样跑
        Serial.printf("%s: 存储没开，配置存不下\n", TAG);
        return false;
    }

    // 整包写进去；Preferences 自己会落盘，不用再喊一次提交
    const size_t wrote = s_prefs.putBytes(AUTO_NVS_KEY, &blob, sizeof(blob));
    if (wrote != sizeof(blob)) {
        Serial.printf("%s: 配置存不下（写了 %u / 要 %u）\n",
                      TAG, (unsigned)wrote, (unsigned)sizeof(blob));
        return false;
    }

    char buf[320];
    if (automation_cfg_json(buf, sizeof(buf)) > 0) {
        Serial.printf("%s: 配置已存: %s\n", TAG, buf);
    }
    return true;
}

bool automation_set_enabled(bool enabled) {
    s_cfg.enabled = enabled;

    Serial.printf("%s: 自动模式 %s\n", TAG, enabled ? "ON" : "OFF");
    return true;
}

bool automation_is_enabled(void) {
    return s_cfg.enabled;
}

// 给的是里头地址，改完记得喊存
automation_cfg_t *automation_get_cfg(void) {
    return &s_cfg;
}

// 按名字改一条线
bool automation_set_threshold(const char *key, float value) {
    if (key == NULL) {
        return false;
    }

    bool        ok       = true;
    bool        log_bool = false;
    bool        bool_val = false;
    const char *why      = "越界或跟另一条线打架";

    // 不像数的值直接挡掉
    if (!isfinite(value)) {
        Serial.printf("%s: cfg.%s 不是个数，拒掉\n", TAG, key);
        return false;
    }

    if (strcmp(key, "enabled") == 0) {
        s_cfg.enabled = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.enabled;
    } else if (strcmp(key, "light_on_lux") == 0) {
        // 开灯线必须比关灯线低
        if ((value <= 0.0f) || (value >= s_cfg.light_off_lux)) {
            ok = false;
        } else {
            s_cfg.light_on_lux = value;
        }
    } else if (strcmp(key, "light_off_lux") == 0) {
        // 两条线中间那段先不动
        if (value <= s_cfg.light_on_lux) {
            ok = false;
        } else {
            s_cfg.light_off_lux = value;
        }
    } else if (strcmp(key, "temp_fan_on_c") == 0) {
        // 开风扇线必须比关的高
        if ((value < -10.0f) || (value > 60.0f) || (value <= s_cfg.temp_fan_off_c)) {
            ok = false;
        } else {
            s_cfg.temp_fan_on_c = value;
        }
    } else if (strcmp(key, "temp_fan_off_c") == 0) {
        // 关风扇线得低一些
        if (value >= s_cfg.temp_fan_on_c) {
            ok = false;
        } else {
            s_cfg.temp_fan_off_c = value;
        }
    } else if (strcmp(key, "rain_pct") == 0) {
        if ((value < 0.0f) || (value > 100.0f)) {
            ok = false;
        } else {
            s_cfg.rain_pct = value;
        }
    } else if (strcmp(key, "fan_auto_speed") == 0) {
        if ((value < 0.0f) || (value > 100.0f)) {
            ok = false;
        } else {
            // 四舍五入成整数
            s_cfg.fan_auto_speed = (uint8_t)(value + 0.5f);
        }
    } else if (strcmp(key, "auto_light_enable") == 0) {
        s_cfg.auto_light_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_light_enable;
    } else if (strcmp(key, "auto_temp_enable") == 0) {
        s_cfg.auto_temp_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_temp_enable;
    } else if (strcmp(key, "auto_rain_enable") == 0) {
        s_cfg.auto_rain_enable = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_rain_enable;
    } else if (strcmp(key, "auto_window_reopen") == 0) {
        s_cfg.auto_window_reopen = (value != 0.0f);
        log_bool = true;
        bool_val = s_cfg.auto_window_reopen;
    } else {
        // 这个名字不认识
        why = "名字不认识";
        ok  = false;
    }

    if (ok) {
        if (log_bool) {
            Serial.printf("%s: cfg.%s = %s\n", TAG, key, bool_val ? "true" : "false");
        } else {
            Serial.printf("%s: cfg.%s = %.2f\n", TAG, key, (double)value);
        }
    } else {
        Serial.printf("%s: cfg.%s = %.2f 被拒（%s）\n", TAG, key, (double)value, why);
    }

    // 这儿先不存，等外面喊存
    return ok;
}

// 几条线拼成 JSON
int automation_cfg_json(char *buf, size_t len) {
    automation_cfg_t snap;

    if ((buf == NULL) || (len == 0)) {
        Serial.printf("%s: cfg_json 缓冲区不对\n", TAG);
        return 0;
    }

    cfg_snapshot(&snap);

    // 字段名和顺序照原版那份来，手机按这些名字解
    const int n = snprintf(buf, len,
                           "{\"enabled\":%s,\"light_on_lux\":%.2f,\"light_off_lux\":%.2f,"
                           "\"temp_fan_on_c\":%.2f,\"temp_fan_off_c\":%.2f,\"rain_pct\":%.2f,"
                           "\"fan_auto_speed\":%u,\"auto_light_enable\":%s,"
                           "\"auto_temp_enable\":%s,\"auto_rain_enable\":%s,"
                           "\"auto_window_reopen\":%s}",
                           snap.enabled ? "true" : "false",
                           (double)snap.light_on_lux, (double)snap.light_off_lux,
                           (double)snap.temp_fan_on_c, (double)snap.temp_fan_off_c,
                           (double)snap.rain_pct, (unsigned)snap.fan_auto_speed,
                           snap.auto_light_enable ? "true" : "false",
                           snap.auto_temp_enable ? "true" : "false",
                           snap.auto_rain_enable ? "true" : "false",
                           snap.auto_window_reopen ? "true" : "false");

    if ((n <= 0) || (n >= (int)len)) {
        // 装不下就整条作废，别发出去半截 JSON
        Serial.printf("%s: cfg_json 缓冲区太小（要 %d 字节，只有 %u）\n",
                      TAG, n, (unsigned)len);
        return 0;
    }

    return n;
}

// 告诉联动刚有人动手
void automation_notify_manual(device_id_t id) {
    const uint32_t now_ms = millis();

    if (id == DEV_COUNT) {
        // 表尾这个值代表全部
        for (int i = 0; i < DEV_COUNT; i++) {
            s_last_manual_ms[i] = now_ms;
        }
    } else if (((int)id >= 0) && (id < DEV_COUNT)) {
        s_last_manual_ms[id] = now_ms;
    }

    Serial.printf("%s: 手动保护开始（%s），%u ms 内自动不碰\n", TAG,
                  (id == DEV_COUNT) ? "全部" : device_id_name(id),
                  (unsigned)AUTO_MANUAL_GUARD_MS);
}

// 主循环隔会儿喊一次
void automation_tick(const sensor_data_t *d) {
    if (d == NULL) {
        return;
    }

    if (!s_inited) {
        // 主循环要是漏了在 setup 里喊 init，这儿补一次，省得联动整个不干活
        automation_init();
    }

    // 主循环跑得比原版勤，这儿自己按 500ms 节流，跟原版主循环一个节拍
    const uint32_t now_ms = millis();
    if ((uint32_t)(now_ms - s_last_tick_ms) < AUTO_TICK_MS) {
        return;
    }
    s_last_tick_ms = now_ms;

    automation_cfg_t cfg;

    cfg_snapshot(&cfg);

    if (!cfg.enabled) {
        // 总开关关着就什么都不管；原版这儿是调试级日志，这边不打，免得刷屏
        return;
    }

    // 头一条，看光照暗不暗
    if (cfg.auto_light_enable && light_data_valid(d)) {
        // 板子上没 BH1750，lux 是光敏百分比折出来的等效值，阈值还是原来那两条
        const float lux = light_eq_lux(d);

        // 客厅那盏灯
        if (!device_get_power(DEV_LED_LIVING) && (lux < cfg.light_on_lux)) {
            if (guard_ok(DEV_LED_LIVING, now_ms)) {
                Serial.printf("%s: rule1: lux %.1f < on %.1f & living LED off -> LED ON\n",
                              TAG, (double)lux, (double)cfg.light_on_lux);
                device_set_power(DEV_LED_LIVING, true, SRC_AUTO);
            }
            // 手动动过的先歇一会儿，别跟人抢；原版这儿也是调试级日志
        } else if (device_get_power(DEV_LED_LIVING) && (lux > cfg.light_off_lux)) {
            if (guard_ok(DEV_LED_LIVING, now_ms)) {
                Serial.printf("%s: rule1: lux %.1f > off %.1f & living LED on -> LED OFF\n",
                              TAG, (double)lux, (double)cfg.light_off_lux);
                device_set_power(DEV_LED_LIVING, false, SRC_AUTO);
            }
        }

#if BSP_SERVO_CURTAIN_ENABLE
        // 窗帘和灯正相反
        const uint8_t curtain_pos = device_get_level(DEV_CURTAIN);
        if ((lux < cfg.light_on_lux) && (curtain_pos > 0)) {
            if (guard_ok(DEV_CURTAIN, now_ms)) {
                Serial.printf("%s: rule1: lux %.1f < on %.1f & curtain pos %u -> CURTAIN CLOSE(0)\n",
                              TAG, (double)lux, (double)cfg.light_on_lux, (unsigned)curtain_pos);
                device_set_level(DEV_CURTAIN, 0, SRC_AUTO);
            }
        } else if ((lux > cfg.light_off_lux) && (curtain_pos < 100)) {
            if (guard_ok(DEV_CURTAIN, now_ms)) {
                Serial.printf("%s: rule1: lux %.1f > off %.1f & curtain pos %u -> CURTAIN OPEN(100)\n",
                              TAG, (double)lux, (double)cfg.light_off_lux, (unsigned)curtain_pos);
                device_set_level(DEV_CURTAIN, 100, SRC_AUTO);
            }
        }
#else
        // 窗帘舵机写死停用了，这条联动跟着一起关掉
#endif
    }

    // 热了开风扇，凉了关
    if (cfg.auto_temp_enable && d->valid_temp) {
        if (!device_get_power(DEV_FAN) && (d->temperature > cfg.temp_fan_on_c)) {
            if (guard_ok(DEV_FAN, now_ms)) {
                Serial.printf("%s: rule2: temp %.1f > on %.1f & fan off -> FAN ON %u%%\n",
                              TAG, (double)d->temperature, (double)cfg.temp_fan_on_c,
                              (unsigned)cfg.fan_auto_speed);
                device_set_level(DEV_FAN, cfg.fan_auto_speed, SRC_AUTO);
            }
            // 手动动过的先歇一会儿
        } else if (device_get_power(DEV_FAN) && (d->temperature < cfg.temp_fan_off_c)) {
            if (guard_ok(DEV_FAN, now_ms)) {
                Serial.printf("%s: rule2: temp %.1f < off %.1f & fan on -> FAN OFF\n",
                              TAG, (double)d->temperature, (double)cfg.temp_fan_off_c);
                device_set_power(DEV_FAN, false, SRC_AUTO);
            }
        }
    }

    // 下雨就把窗关上
    if (cfg.auto_rain_enable) {
        const bool raining = (d->rain_pct > cfg.rain_pct);
        // 重开要更干一档，留迟滞免得小雨在阈值附近反复开关
        const float reopen_line = cfg.rain_pct - AUTO_RAIN_REOPEN_HYST_PCT;
        const bool  dry_enough  = (d->rain_pct <= ((reopen_line > 0.0f) ? reopen_line : 0.0f));
        const bool  window_open = device_get_power(DEV_WINDOW);

        if (raining && window_open) {
            // 安全优先：下雨关窗不理会手动保护，马上关
            Serial.printf("%s: rule3: rain %.1f%% > %.1f%% & window open -> WINDOW CLOSE (safety, guard bypassed)\n",
                          TAG, (double)d->rain_pct, (double)cfg.rain_pct);
            device_set_power(DEV_WINDOW, false, SRC_AUTO);
        } else if (!raining && dry_enough && !window_open && cfg.auto_window_reopen) {
            // 重开是舒适动作，手动保护照旧
            if (guard_ok(DEV_WINDOW, now_ms)) {
                Serial.printf("%s: rule3: rain %.1f%% <= %.1f%% (hyst) & window closed -> WINDOW REOPEN\n",
                              TAG, (double)d->rain_pct, (double)reopen_line);
                device_set_power(DEV_WINDOW, true, SRC_AUTO);
            }
            // 手动动过的先歇一会儿，原版这儿也是调试级日志
        }
    }
}
