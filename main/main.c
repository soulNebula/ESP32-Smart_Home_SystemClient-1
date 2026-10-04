/**
 * @file  main.c
 * @brief ESP32-S3 智能家居 —— 应用入口 / 任务拓扑 / 事件分发
 *
 * ===========================================================================
 *  需求 → 模块 对照表
 * ---------------------------------------------------------------------------
 *  ① 光照超过/低于阈值 → 控制 LED 或窗帘 ......... automation.c 规则1
 *  ② 温湿度采集 + OLED 显示 + 高温开风扇 ......... sensor.c / oled.c / automation.c 规则2
 *  ③ 雨滴传感器 → 控制窗户开关 ................... sensor.c / automation.c 规则3
 *  ④ 厨房/客厅/浴室/卧室 LED 开关 + 语音控制 ..... led.c / device_model.c / voice.c
 *  ⑤ 舵机开合门 .................................. servo.c (SERVO_DOOR)
 *  ⑥ 语音交互：控制所有设备 + 播报温湿度 ......... voice.c + voice_on_cmd()
 *  ⑦ 终端 + 手机 App 控制所有设备 ................ 本文件的串口调试台 / mqtt_app.c
 * ===========================================================================
 *
 *  【任务拓扑】
 *    app_main                 : 初始化，创建下面 2 个常驻任务后返回
 *    app_loop_task   (1Hz/2Hz): automation_tick + OLED 刷新 + 状态灯
 *    serial_console_task      : 串口调试台（配件没到时模拟语音/手机指令）
 *    sensor 采样任务           : 由 sensor_start_auto() 创建
 *    key 扫描任务              : 由 key_init() 创建
 *    voice 接收任务            : 由 voice_init() 创建
 *    MQTT 客户端任务           : 由 esp-mqtt 内部创建
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- BSP ---- */
#include "board.h"
#include "board_config.h"
#include "led.h"
#include "key.h"
#include "servo.h"
#include "fan.h"
#include "sensor.h"
#include "oled.h"
#include "voice.h"
#include "voice_esp_sr.h"
#include "adc_bus.h"
#include "adkey.h"
#include "i2s_mic.h"

/* ---- App ---- */
#include "device_model.h"
#include "automation.h"
#include "wifi_sta.h"
#include "mqtt_app.h"
#include "ble_app.h"
#include "selftest.h"
#include "astra_glue.h"

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#endif

static const char *TAG = "MAIN";

/* ========================================================================= */
/*  1. 用户配置的回调：语音指令 → 设备动作                                    */
/* ========================================================================= */

/**
 * @brief 语音指令翻译成设备动作
 *
 * ★ 这是"语音"和"设备"之间唯一的耦合点。
 *   无论指令来自真实语音模块（voice.c 的 UART 解析）还是 voice_inject_cmd()
 *   手动注入（按键 / 串口调试台 / MQTT），都会走到这里 —— 下游完全一致。
 *   以后换成 ESP-SR 离线识别或云端识别，本函数一行都不用改。
 */
static void voice_on_cmd(voice_cmd_t cmd, void *user)
{
    (void)user;
    const ctrl_source_t src = SRC_VOICE;

    switch (cmd) {
    /* ---- 单个房间灯 ---- */
    case VOICE_CMD_LED_LIVING_ON:   device_set_power(DEV_LED_LIVING,  true,  src); break;
    case VOICE_CMD_LED_LIVING_OFF:  device_set_power(DEV_LED_LIVING,  false, src); break;
    case VOICE_CMD_LED_KITCHEN_ON:  device_set_power(DEV_LED_KITCHEN, true,  src); break;
    case VOICE_CMD_LED_KITCHEN_OFF: device_set_power(DEV_LED_KITCHEN, false, src); break;
    case VOICE_CMD_LED_BEDROOM_ON:  device_set_power(DEV_LED_BEDROOM, true,  src); break;
    case VOICE_CMD_LED_BEDROOM_OFF: device_set_power(DEV_LED_BEDROOM, false, src); break;
    case VOICE_CMD_LED_BATH_ON:     device_set_power(DEV_LED_BATH,    true,  src); break;
    case VOICE_CMD_LED_BATH_OFF:    device_set_power(DEV_LED_BATH,    false, src); break;

    /* ---- 全部灯 ---- */
    case VOICE_CMD_LED_ALL_ON:
    case VOICE_CMD_LED_ALL_OFF: {
        bool on = (cmd == VOICE_CMD_LED_ALL_ON);
        device_set_power(DEV_LED_LIVING,  on, src);
        device_set_power(DEV_LED_KITCHEN, on, src);
        device_set_power(DEV_LED_BEDROOM, on, src);
        device_set_power(DEV_LED_BATH,    on, src);
        break;
    }

    /* ---- 风扇 ---- */
    case VOICE_CMD_FAN_ON:  device_set_power(DEV_FAN, true,  src); break;
    case VOICE_CMD_FAN_OFF: device_set_power(DEV_FAN, false, src); break;

    /* ---- 窗户 ---- */
    case VOICE_CMD_WINDOW_OPEN:  device_set_power(DEV_WINDOW, true,  src); break;
    case VOICE_CMD_WINDOW_CLOSE: device_set_power(DEV_WINDOW, false, src); break;

    /* ---- 门 ---- */
    case VOICE_CMD_DOOR_OPEN:  device_set_power(DEV_DOOR, true,  src); break;
    case VOICE_CMD_DOOR_CLOSE: device_set_power(DEV_DOOR, false, src); break;

    /* ---- 窗帘 ---- */
    case VOICE_CMD_CURTAIN_OPEN:  device_set_power(DEV_CURTAIN, true,  src); break;
    case VOICE_CMD_CURTAIN_CLOSE: device_set_power(DEV_CURTAIN, false, src); break;

    /* ---- 查询播报（需求 ⑥："或者播报当前温湿度"） ---- */
    case VOICE_CMD_QUERY_TEMP: {
        const sensor_data_t *d = sensor_get_last();
        if (d->valid_temp) {
            voice_speak_temp(d->temperature, -1.0f);   /* 只播温度 */
        } else {
            voice_speak("温度传感器未连接");
        }
        break;
    }
    case VOICE_CMD_QUERY_HUMI: {
        const sensor_data_t *d = sensor_get_last();
        if (d->valid_temp) {
            voice_speak_temp(-100.0f, d->humidity);    /* 只播湿度 */
        } else {
            voice_speak("湿度传感器未连接");
        }
        break;
    }
    case VOICE_CMD_QUERY_LIGHT: {
        const sensor_data_t *d = sensor_get_last();
        char buf[64];
        snprintf(buf, sizeof(buf), "当前光照百分之%d", (int)(d->light_pct + 0.5f));
        voice_speak(buf);
        break;
    }
    case VOICE_CMD_QUERY_ALL:
    case VOICE_CMD_QUERY_STATUS: {
        const sensor_data_t *d = sensor_get_last();
        if (d->valid_temp) {
            voice_speak_temp(d->temperature, d->humidity);
        } else {
            voice_speak("温湿度传感器未连接");
        }
        break;
    }

    /* ---- 自动化总开关 ---- */
    case VOICE_CMD_AUTO_ON:
        automation_set_enabled(true);
        automation_save();
        voice_speak("已开启自动模式");
        break;
    case VOICE_CMD_AUTO_OFF:
        automation_set_enabled(false);
        automation_save();
        voice_speak("已关闭自动模式");
        break;

    case VOICE_CMD_NONE:
    default:
        return;   /* 不汇报、不刷屏 */
    }

    /* 本地事件也上报给 MQTT，方便手机端看到"是谁开的灯" */
    mqtt_publish_event(voice_cmd_name(cmd));
    voice_speak("好的");
}

/* ========================================================================= */
/*  2. 用户配置的回调：按键 → 设备动作                                        */
/* ========================================================================= */

/* ---- 五位 AD 键盘事件回显（导航由 astra UI 的 key_down_cb 瞬时查询驱动，
 *       本回调只做诊断日志，不处理业务） ---- */
static const char *const s_adkey_names[ADKEY_NUM] = { "1", "2", "3", "4", "OK" };

static void adkey_on_event(adkey_id_t id, adkey_event_t ev, void *user)
{
    (void)user;
    static const char *const s_ev_names[] = { "DOWN", "UP", "CLICK", "LONG" };

    /* ★ 打"真正触发的那次读数"（adkey_last_mv），不是 adkey_raw_mv()：
     *   回调跑到这里时按键多半已经松开，实时读数早就变回空闲 3128mV 了，
     *   以前用实时值导致日志里全是 mv=3128，根本没法用它标定。 */
    ESP_LOGI(TAG, "[ADKEY] %s %s (触发读数=%dmV, 当前=%dmV)",
             s_adkey_names[id], s_ev_names[ev], adkey_last_mv(), adkey_raw_mv());
}

/**
 * @brief 按键动作映射（KEY1 已弃用/NC，只剩 KEY2 一个数字键）
 *
 *   KEY2 单击 → 切换【自动模式】开/关
 *   KEY2 长按 → 切换【风扇】开/关
 */
static void key_on_event(key_id_t id, key_event_t ev, void *user)
{
    (void)user;

    /* 自检模式内：数字按键不参与（菜单由五位键盘驱动） */
    if (selftest_is_active()) {
        return;
    }

    if (id == KEY_ID_1) {
        if (ev == KEY_EVENT_CLICK) {
            ESP_LOGI(TAG, "[KEY1] click  -> toggle living room light");
            device_toggle(DEV_LED_LIVING, SRC_LOCAL_KEY);
            mqtt_publish_event("key1_click");
        } else if (ev == KEY_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "[KEY1] long   -> ALL OFF");
            device_all_off(SRC_LOCAL_KEY);
            mqtt_publish_event("key1_long_all_off");
        }
    } else if (id == KEY_ID_2) {
        if (ev == KEY_EVENT_CLICK) {
            bool now = !automation_is_enabled();
            ESP_LOGI(TAG, "[KEY2] click  -> auto mode %s", now ? "ON" : "OFF");
            automation_set_enabled(now);
            automation_save();
            mqtt_publish_event(now ? "key2_auto_on" : "key2_auto_off");
        } else if (ev == KEY_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "[KEY2] long   -> toggle fan");
            device_toggle(DEV_FAN, SRC_LOCAL_KEY);
            mqtt_publish_event("key2_long_fan_toggle");
        }
    }
}

/* ========================================================================= */
/*  3. 主循环任务：自动联动 + OLED 刷新 + 状态灯                              */
/* ========================================================================= */

static void app_loop_task(void *arg)
{
    (void)arg;

    const uint32_t tick_ms        = 500;

    /* 传感器周期上报：CONFIG_APP_MQTT_PUBLISH_SENSOR_MS（menuconfig，默认 2000ms）。
     * ★ 这一条以前是漏的 —— Kconfig 里有这个选项，但没人用它，
     *   结果 <base>/sensor 只在 MQTT 刚连上和收到 /get 时才发一次，没有周期上报。 */
    const uint32_t mqtt_sensor_period_ms = CONFIG_APP_MQTT_PUBLISH_SENSOR_MS;
    uint32_t mqtt_sensor_acc = mqtt_sensor_period_ms;   /* 首次立即发一次 */

    bool last_mqtt = false;

    for (;;) {
        /* ---- 自动联动：需求 ①②③ ---- */
        automation_tick(sensor_get_last());

        /* ---- 周期上报传感器数据（需求 ②⑦：手机端要能看到实时温湿度） ---- */
        mqtt_sensor_acc += tick_ms;
        if (mqtt_sensor_acc >= mqtt_sensor_period_ms) {
            mqtt_sensor_acc = 0;
            if (mqtt_is_connected()) {
                mqtt_publish_sensor(sensor_get_last());
            }
        }

        /* ---- 状态灯跟随网络状态 ---- */
        bool mqtt_now = mqtt_is_connected();
        if (mqtt_now != last_mqtt) {
            last_mqtt = mqtt_now;
            if (mqtt_now) {
                led_status_set(LED_STATUS_MQTT_OK);
            } else if (wifi_is_connected()) {
                led_status_set(LED_STATUS_WIFI_OK);
            }
        }

        /* ---- 自检模式：状态机推进（进入/退出由五位键盘 OK 长按触发） ---- */
        if (selftest_is_active()) {
            selftest_tick();
        }

        /* ---- OLED：已由 astra UI 任务全权渲染（u8g2 画布 + oled_write_page） ---- */

        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}

/* ========================================================================= */
/*  4. 串口调试台 —— 配件没到货时，用它模拟语音和手机 App 的全部指令          */
/* ========================================================================= */
#if CONFIG_APP_SERIAL_DEBUG_ENABLE

/* ===========================================================================
 *  日志级别控制（把刷屏的日志"隔离"，要用时再放出来）
 * ===========================================================================
 *  BLE 协议栈（NimBLE）在连接/广播/notify 时会持续打 INFO 日志，例如
 *      I (252004) NimBLE: GATT procedure initiated: notify;
 *      I (253704) NimBLE: GAP procedure initiated: advertise;
 *  几秒钟就把串口刷满，导致根本没法在调试台里敲命令。
 *
 *  这里用 esp_log_level_set() 在【运行时】压制它们，不修改任何模块的代码：
 *      log quiet   → 把 BLE / 无线协议栈的 tag 压到 WARN（推荐）
 *      log all     → 全部恢复 INFO（要排查 BLE 时用）
 *      log w       → 全局只留 WARN/ERROR（最安静）
 *      log d NimBLE→ 单独把某个 tag 开到 DEBUG
 *
 *  优先级：某个 tag 被单独设置过之后，会覆盖 "*" 的全局设置。
 *  所以 quiet/all 都要把 noisy 列表里的 tag 一起设置，否则改不回来。
 * =========================================================================== */

/* 会持续刷屏的 tag —— 以后加了新的噪音源，往这里加一行即可 */
static const char *const s_noisy_tags[] = {
    "NimBLE",             /* BLE 协议栈：GATT/GAP procedure 日志，最大噪音源 */
    "ble_app",            /* BLE 应用层 */
    "BLE",
    "wifi",               /* 无线相关（多为启动期一次性） */
    "net80211",
    "phy_init",
    "esp_netif_handlers",
    "spi_flash",
    "heap_init",
};
#define NOISY_TAG_CNT (sizeof(s_noisy_tags) / sizeof(s_noisy_tags[0]))

/* 只压 BLE 三个 tag —— 开机默认用这个，保留 wifi/app 的 INFO 方便排错 */
static const char *const s_ble_tags[] = { "NimBLE", "ble_app", "BLE" };
#define BLE_TAG_CNT (sizeof(s_ble_tags) / sizeof(s_ble_tags[0]))

static void log_set_tags(const char *const *tags, size_t cnt, esp_log_level_t lv)
{
    for (size_t i = 0; i < cnt; i++) {
        esp_log_level_set(tags[i], lv);
    }
}

static esp_log_level_t log_level_from_char(char c)
{
    switch (c) {
    case 'n': return ESP_LOG_NONE;
    case 'e': return ESP_LOG_ERROR;
    case 'w': return ESP_LOG_WARN;
    case 'i': return ESP_LOG_INFO;
    case 'd': return ESP_LOG_DEBUG;
    case 'v': return ESP_LOG_VERBOSE;
    default:  return ESP_LOG_INFO;
    }
}

static const char *log_level_name(esp_log_level_t lv)
{
    switch (lv) {
    case ESP_LOG_NONE:    return "NONE";
    case ESP_LOG_ERROR:   return "ERROR";
    case ESP_LOG_WARN:    return "WARN";
    case ESP_LOG_INFO:    return "INFO";
    case ESP_LOG_DEBUG:   return "DEBUG";
    case ESP_LOG_VERBOSE: return "VERBOSE";
    default:              return "?";
    }
}

static void console_print_help(void)
{
    printf("\n");
    printf("==================== 串口调试台 ====================\n");
    printf("  status                  查看全部设备 + 传感器状态\n");
    printf("  on   <dev>              开   (led_living/led_kitchen/led_bedroom/\n");
    printf("                                led_bath/fan/window/door/curtain/all)\n");
    printf("  off  <dev>              关\n");
    printf("  toggle <dev>            翻转\n");
    printf("  set  <dev> <0-100>      调档（灯=亮度 风扇=转速 舵机=开合位置）\n");
    printf("  color <dev> <r> <g> <b> 设灯带颜色\n");
    printf("  open  <dev>  / close <dev>\n");
    printf("  auto <on|off>           自动联动总开关\n");
    printf("  cfg                     查看自动联动阈值\n");
    printf("  cfg <key> <value>       改阈值（如 cfg temp_fan_on_c 30）\n");
    printf("  say  <voice_cmd>         ★模拟「说了一句话」，走完整语音链路\n");
    printf("                          指令名见下方 help-voice\n");
    printf("  help-voice              列出所有语音指令名\n");
    printf("  mic                     ★INMP441 麦克风实时电平（不用说话就能看它工作没有）\n");
    printf("  mic dump [秒]           采一段原始音频打印波形统计（判断接线对错）\n");
    printf("  voice-test              ★ESP-SR：打印唤醒词 + 完整命令词表\n");
    printf("  test                     ★按键自检模式（五位键盘 OK 长按 2 秒也可进入）\n");
    printf("  test run / next / off    自检：执行当前项 / 下一项 / 退出\n");
    printf("  keyscan <秒>             ★五键 AD 键盘探测：按 1→2→3→4→OK，看哪个脚变化\n");
    printf("  ---- 日志隔离（串口被刷屏时用）----\n");
    printf("  log quiet               ★把 BLE/无线协议栈压到 WARN，其它日志照常\n");
    printf("  log all                 恢复全部 INFO（要排查 BLE 时用）\n");
    printf("  log w                   全局只留 WARN/ERROR（最安静）\n");
    printf("  log n                   全局静音（连 ERROR 都不打）\n");
    printf("  log <n|e|w|i|d|v> <tag> 单独设置某个 tag，如 log d NimBLE\n");
    printf("  help                    显示本帮助\n");
    printf("====================================================\n\n");
}

/* ------------------------------------------------------------------ */
/*  五键 AD 键盘探测（keyscan）：扫描候选引脚，值变化就打印            */
/* ------------------------------------------------------------------ */
/* 候选：IO3(ADC1_CH2) / IO12·IO13·IO14(ADC2) / IO38~42(数字上拉)。
 * 用法：串口敲 `keyscan 30`，然后按 1→2→3→4→OK 的顺序按一遍，
 * 哪个引脚的值跟着变，键盘就接在哪个脚上；各键对应电压就是键值表。 */
static void console_keyscan(int seconds)
{
    /* 键盘已确认接在 GPIO10（ADC1_CH9），所以这里只测 IO10，不再扫 IO38~42 */
    printf("  [keyscan] 监听 %d 秒：IO10 电压一变就打印（变化阈值 8mV，50ms 采样）。\n", seconds);
    printf("  [keyscan] 空闲 ≈3128mV；按住某个键会跳到对应分压档。\n");
    printf("  [keyscan] 键档参考：OK≈0mV / 3≈615 / 1≈1380 / 4≈1968 / 2≈2638 mV\n");

    const int64_t t0     = esp_timer_get_time();
    const int64_t end_us = t0 + (int64_t)seconds * 1000000LL;
    int last_mv = -1;
    int min_mv  = 100000, max_mv = -1;

    while (esp_timer_get_time() < end_us) {
        const int mv = adkey_raw_mv();
        if (mv < min_mv) { min_mv = mv; }
        if (mv > max_mv) { max_mv = mv; }

        /* 只在"变化 >= 8mV"时打印：短按（几十毫秒）也不会漏掉 */
        if (last_mv < 0 || mv - last_mv >= 8 || last_mv - mv >= 8) {
            printf("  [keyscan] t=%6lldms  IO10=%4dmV%s\n",
                   (long long)((esp_timer_get_time() - t0) / 1000), mv,
                   (last_mv < 0) ? "" : ((mv < last_mv) ? "  ↓" : "  ↑"));
            last_mv = mv;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("  [keyscan] 结束：最低 %dmV / 最高 %dmV（空闲基线 ≈3128mV）\n", min_mv, max_mv);
    printf("  [keyscan] done\n");
}

static void console_print_status(void)
{
    const sensor_data_t *s = sensor_get_last();
    char line[160];

    printf("\n---- 设备状态 ----\n");
    for (int i = 0; i < DEV_COUNT; i++) {
        device_id_t id = (device_id_t)i;
        printf("  %-12s power=%-3s level=%3d%%\n",
               device_id_name(id),
               device_get_power(id) ? "ON" : "OFF",
               device_get_level(id));
    }

    printf("\n---- 传感器 ----\n");
    sensor_format_line(s, line, sizeof(line));
    printf("  %s\n", line);
    printf("  温度有效=%s  光照模式=%s\n",
           s->valid_temp ? "yes" : "no",
           s->light_is_bh1750 ? "BH1750(真实lux)" : "光敏电阻(折算)");

    printf("\n---- 网络 ----\n");
    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    printf("  WiFi=%s  IP=%s  RSSI=%d   MQTT=%s\n",
           wifi_is_connected() ? "OK" : "down", ip, wifi_get_rssi(),
           mqtt_is_connected() ? "OK" : "down");

    printf("\n---- 自动联动 ----\n");
    char cfg[320];
    automation_cfg_json(cfg, sizeof(cfg));
    printf("  %s\n\n", cfg);
}

static void console_print_voice_cmds(void)
{
    printf("\n---- 可用语音指令名 ----\n");
    for (int i = 1; i < VOICE_CMD_MAX; i++) {
        printf("  %-22s", voice_cmd_name((voice_cmd_t)i));
        if ((i % 3) == 0) printf("\n");
    }
    printf("\n\n用法：say led_living_on\n\n");
}

/* ------------------------------------------------------------------ */
/*  mic：INMP441 麦克风自检（★ 用户不需要说话就能确认麦克风工作没有）  */
/* ------------------------------------------------------------------ */
/*  为什么要有这条命令：
 *    麦克风这类"音频"器件最难排错 —— 接线错了不会报错，只会一直静音。
 *    本命令直接读 I²S 的电平，把"有没有信号"变成几个能看懂的数字：
 *        · 全 0 / 峰峰值 ≈ 0        → 麦克风没接，或 SD 接错
 *        · 数值恒定不变              → SCK/WS 接反，或 L/R 悬空
 *        · 安静时 rms 几十~几百      → 正常工作（底噪）
 *        · 对着说话 peak 明显变大    → 一定在工作
 *    `mic dump 3` 会采 3 秒并打印每 500ms 一行的 min/max/avg/peak/clip。 */
static void console_mic_level(void)
{
    /* 先主动读一小段，保证打印的是"此刻"的电平，而不是很久以前那次快照 */
    int16_t probe[320] = { 0 };     /* 20ms @16KHz */
    size_t  got = 0;
    const esp_err_t err = i2s_mic_read(probe, sizeof(probe) / sizeof(probe[0]), &got, 300);

    i2s_mic_level_t lv;
    i2s_mic_read_level(&lv);

    printf("\n---- INMP441 麦克风自检 ----\n");
    printf("  接线提示：INMP441 是 I²S【不是 I²C】\n");
    printf("      SCK=GPIO%d   WS=GPIO%d   SD=GPIO%d\n",
           (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);
    printf("      L/R=GND（单麦必须固定接一端）   VDD=3V3（★不要接5V）\n");
    printf("  ---- 实时电平 ----\n");
    printf("  最近一次读取 : %u 个采样点（%s）\n",
           (unsigned)lv.last_samples, (err == ESP_OK) ? "读满" : esp_err_to_name(err));
    printf("  rms          = %d   （安静房间几十~几百是正常底噪）\n", lv.rms);
    printf("  peak         = %d   （对着麦克风说话应明显变大）\n", lv.peak);
    printf("  峰峰值       = %d   （★是否在拾音主要看它：≈0 = 没在工作）\n", lv.peak_to_peak);
    printf("  直流偏置     = %d   （理想接近 0；很大说明是直流不是声音）\n", lv.dc);
    if (lv.db_x10 <= -9990) {
        printf("  dBFS         = 无信号\n");
    } else {
        printf("  dBFS         = %d.%d\n", lv.db_x10 / 10,
               (lv.db_x10 < 0 ? -lv.db_x10 : lv.db_x10) % 10);
    }
    printf("  累计读取次数 = %u， 削顶采样 = %u\n",
           (unsigned)lv.read_count, (unsigned)lv.clip);
    printf("  就绪(采到有效音频) = %s\n", i2s_mic_is_ready() ? "是 ★" : "否");

    printf("  ---- 结论 ----\n");
    if (lv.read_count == 0) {
        printf("  【I²S 还没读到任何数据】—— 检查 VDD/GND 是否接好\n");
    } else if (lv.peak_to_peak <= 32 && lv.peak <= 32) {
        printf("  【没采到有效音频】—— 检查 SD/SCK/WS 三根线、L/R 是否接了 GND\n");
    } else {
        printf("  【麦克风在工作】—— 对着它说话再敲一次 mic，peak 应明显变大\n");
    }
    printf("  （想看波形统计：mic dump 3）\n\n");
}

/** 把 "all" 展开成 4 个灯 + 风扇 + 舵机 */
static void console_dev_action(const char *dev, const char *action, int value)
{
    if (strcmp(dev, "all") == 0) {
        if (strcmp(action, "off") == 0) {
            device_all_off(SRC_MQTT);
        } else if (strcmp(action, "on") == 0) {
            /* ★ "全开" = 4 路灯带 + 风扇，【不】开窗/门/窗帘（把门全开是安全隐患）。
             *   必须和 MQTT 的 {"dev":"all","action":"on"} 保持完全一致，
             *   见 mqtt_app.c::handle_cmd()。 */
            device_set_power(DEV_LED_LIVING,  true, SRC_MQTT);
            device_set_power(DEV_LED_KITCHEN, true, SRC_MQTT);
            device_set_power(DEV_LED_BEDROOM, true, SRC_MQTT);
            device_set_power(DEV_LED_BATH,    true, SRC_MQTT);
            device_set_power(DEV_FAN,         true, SRC_MQTT);
        } else {
            printf("  all 只支持 on / off\n");
        }
        return;
    }

    device_id_t id = device_from_name(dev);
    if (id >= DEV_COUNT) {
        printf("  未知设备: %s（输入 help 看列表）\n", dev);
        return;
    }

    esp_err_t err;
    if (strcmp(action, "on") == 0) {
        err = device_set_power(id, true, SRC_MQTT);
    } else if (strcmp(action, "off") == 0) {
        err = device_set_power(id, false, SRC_MQTT);
    } else if (strcmp(action, "toggle") == 0) {
        err = device_toggle(id, SRC_MQTT);
    } else if (strcmp(action, "open") == 0) {
        err = device_set_level(id, 100, SRC_MQTT);
    } else if (strcmp(action, "close") == 0) {
        err = device_set_level(id, 0, SRC_MQTT);
    } else if (strcmp(action, "set") == 0) {
        err = device_set_level(id, (uint8_t)(value < 0 ? 0 : (value > 100 ? 100 : value)),
                               SRC_MQTT);
    } else {
        printf("  未知动作: %s\n", action);
        return;
    }

    printf("  %s %s -> %s\n", action, dev,
           err == ESP_OK ? "OK" : esp_err_to_name(err));
}

static void console_handle_line(char *line)
{
    /* 去掉行尾 \r\n */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    char cmd[24]  = {0};
    char a1[24]   = {0};
    char a2[24]   = {0};
    int  v1 = 0, v2 = 0;
    int  n = sscanf(line, "%23s %23s %23s %d %d", cmd, a1, a2, &v1, &v2);

    if (n <= 0) {
        return;
    }

    /* sscanf 的坑：命令行里数字是从第 3 个 token 开始的，所以
     *   "set fan 60"        → cmd="set" a1="fan" a2="60"        → 数值在 a2
     *   "color led 1 2 3"   → cmd   a1     a2="1" v1=2 v2=3      → 数值在 a2/v1/v2
     * 统一取成 num0/num1/num2，避免用到没被赋值的 v* */
    int num0 = (n >= 3) ? atoi(a2) : 0;
    int num1 = (n >= 4) ? v1       : 0;
    int num2 = (n >= 5) ? v2       : 0;

    if (strcmp(cmd, "help") == 0) {
        console_print_help();
    } else if (strcmp(cmd, "help-voice") == 0) {
        console_print_voice_cmds();
    } else if (strcmp(cmd, "status") == 0) {
        console_print_status();
    } else if (strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0 ||
               strcmp(cmd, "toggle") == 0 || strcmp(cmd, "open") == 0 ||
               strcmp(cmd, "close") == 0) {
        if (n < 2) { printf("  用法: %s <dev>\n", cmd); return; }
        console_dev_action(a1, cmd, 0);
    } else if (strcmp(cmd, "set") == 0) {
        if (n < 3) { printf("  用法: set <dev> <0-100>\n"); return; }
        console_dev_action(a1, "set", num0);
    } else if (strcmp(cmd, "color") == 0) {
        if (n < 5) { printf("  用法: color <led_xxx> <r> <g> <b>\n"); return; }
        device_id_t id = device_from_name(a1);
        if (id >= DEV_COUNT) { printf("  未知设备: %s\n", a1); return; }
        esp_err_t err = device_set_color(id, (uint8_t)num0, (uint8_t)num1, (uint8_t)num2,
                                         SRC_MQTT);
        printf("  color %s -> %s\n", a1, err == ESP_OK ? "OK" : esp_err_to_name(err));
    } else if (strcmp(cmd, "auto") == 0) {
        bool on = (n >= 2 && (strcmp(a1, "on") == 0 || strcmp(a1, "1") == 0));
        automation_set_enabled(on);
        automation_save();
        printf("  auto mode -> %s\n", on ? "ON" : "OFF");
    } else if (strcmp(cmd, "cfg") == 0) {
        if (n < 3) {
            console_print_status();
        } else {
            esp_err_t err = automation_set_threshold(a1, (float)num0);
            if (err == ESP_OK) {
                automation_save();
                printf("  cfg %s = %d -> OK\n", a1, num0);
            } else {
                printf("  cfg %s = %d -> %s\n", a1, num0, esp_err_to_name(err));
            }
        }
    } else if (strcmp(cmd, "say") == 0) {
        /* ★ 关键：模拟"语音说了一句话"，走的是和真实语音模块完全相同的下游链路 */
        if (n < 2) { printf("  用法: say <voice_cmd>（help-voice 看列表）\n"); return; }
        voice_cmd_t vc = voice_cmd_from_name(a1);
        if (vc == VOICE_CMD_NONE) {
            printf("  未知语音指令: %s（help-voice 看列表）\n", a1);
            return;
        }
        printf("  [模拟语音] %s\n", a1);
        voice_inject_cmd(vc);
    } else if (strcmp(cmd, "adkey") == 0) {
        bool any = false;
        for (int i = 0; i < ADKEY_NUM; i++) {
            if (adkey_is_pressed((adkey_id_t)i)) {
                any = true;
                break;
            }
        }
        printf("  [adkey] IO10=%dmV 按下=%s\n", adkey_raw_mv(), any ? "是" : "否");
    } else if (strcmp(cmd, "mic") == 0) {
        /* ★ "插上线就能测"的主力命令：不用说话就能看出麦克风工作没有 */
        if (n >= 2 && strcmp(a1, "dump") == 0) {
            int secs = (n >= 3) ? atoi(a2) : 3;
            i2s_mic_dump(secs);
        } else {
            console_mic_level();
        }
    } else if (strcmp(cmd, "voice-test") == 0) {
        /* ★ ESP-SR 方案的验证命令：打印唤醒词 + 完整中文命令词表 */
        voice_esp_sr_print_commands();
        if (!voice_esp_sr_is_ready()) {
            printf("  ⚠ ESP-SR 未就绪。若是当前固件没开 ESP-SR 开关，\n");
            printf("    上面的命令词表是【编译进固件的表】，不代表识别已启用。\n");
            printf("    开法：idf.py menuconfig → 智能家居 → 语音识别来源 → 打勾，重新编译烧录。\n\n");
        }
    } else if (strcmp(cmd, "keyscan") == 0) {
        int secs = (n >= 2) ? atoi(a1) : 20;
        if (secs < 1) {
            secs = 1;
        }
        if (secs > 300) {
            secs = 300;
        }
        console_keyscan(secs);
    } else if (strcmp(cmd, "test") == 0) {
        /* 自检模式：无参数 = 进入/退出切换；run = 执行当前项；next = 下一项 */
        if (n >= 2 && strcmp(a1, "run") == 0) {
            selftest_run_current();
            printf("  selftest: run current item\n");
        } else if (n >= 2 && strcmp(a1, "next") == 0) {
            selftest_next();
            printf("  selftest: next item\n");
        } else if (n >= 2 && strcmp(a1, "off") == 0) {
            selftest_exit();
            printf("  selftest: OFF\n");
        } else {
            if (selftest_is_active()) {
                selftest_exit();
                printf("  selftest: OFF\n");
            } else {
                selftest_enter();
                printf("  selftest: ON（3/4 切换 / OK 执行 / OK 长按退出）\n");
            }
        }
    } else if (strcmp(cmd, "log") == 0) {
        if (n < 2) {
            printf("  用法:\n");
            printf("    log quiet               BLE/无线协议栈压到 WARN（推荐）\n");
            printf("    log all                 全部恢复 INFO\n");
            printf("    log w                   全局只留 WARN/ERROR（最安静）\n");
            printf("    log n                   全局静音\n");
            printf("    log <n|e|w|i|d|v> <tag> 单独设置某个 tag\n");
            return;
        }

        if (strcmp(a1, "quiet") == 0) {
            log_set_tags(s_noisy_tags, NOISY_TAG_CNT, ESP_LOG_WARN);
            printf("  [log] BLE/无线相关 tag 已压到 WARN，串口应该安静了\n");
            printf("        要看 BLE 细节：log all  （看完再 log quiet）\n");
        } else if (strcmp(a1, "all") == 0) {
            esp_log_level_set("*", ESP_LOG_INFO);
            /* 被单独设置过的 tag 会覆盖 "*"，所以必须一起改回来 */
            log_set_tags(s_noisy_tags, NOISY_TAG_CNT, ESP_LOG_INFO);
            printf("  [log] 全部 tag -> INFO（BLE 日志会重新开始刷）\n");
        } else {
            const esp_log_level_t lv = log_level_from_char(a1[0]);

            if (n >= 3 && a2[0] != '\0') {
                esp_log_level_set(a2, lv);
                printf("  [log] tag '%s' -> %s\n", a2, log_level_name(lv));
            } else {
                esp_log_level_set("*", lv);
                log_set_tags(s_noisy_tags, NOISY_TAG_CNT, lv);
                printf("  [log] 全部 tag -> %s\n", log_level_name(lv));
            }
        }
    } else {
        printf("  未知命令: %s（输入 help 看帮助）\n", cmd);
    }
}

static void serial_console_task(void *arg)
{
    (void)arg;

    /* 在控制台 UART 上装一个只收不发的驱动，用来读键盘输入。
     * 装驱动后 ESP_LOG 依然可用（走的还是 uart_write_bytes）。 */
    esp_err_t err = uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_install(UART0) failed: %s, 串口调试台不可用",
                 esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    char line[128];
    size_t pos = 0;

    printf("\n\n");
    printf("####################################################\n");
    printf("#  ESP32-S3 智能家居 —— 串口调试台已就绪            #\n");
    printf("#  输入 help 查看所有命令                          #\n");
    printf("#  配件还没到？用 say led_living_on 模拟语音指令    #\n");
    printf("####################################################\n\n");

    for (;;) {
        uint8_t ch;
        int r = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (r != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                console_handle_line(line);
                pos = 0;
            }
        } else if (ch == 0x08 || ch == 0x7F) {   /* 退格 */
            if (pos > 0) {
                pos--;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}
#endif /* CONFIG_APP_SERIAL_DEBUG_ENABLE */

/* ========================================================================= */
/*  5. app_main                                                              */
/* ========================================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "###########################################################");
    ESP_LOGI(TAG, "#  ESP32-S3 智能家居  (ESP-IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "#  build %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "###########################################################");

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
    /* ★ 开机默认把 BLE 协议栈（NimBLE）的 INFO 日志压到 WARN。
     *   否则 BLE 一被手机连接，GATT/GAP procedure 日志会持续刷屏，
     *   串口调试台根本没法用（连命令都敲不进去）。
     *   需要看 BLE 细节时，在调试台敲： log all    ；看完再敲： log quiet */
    log_set_tags(s_ble_tags, BLE_TAG_CNT, ESP_LOG_WARN);
#endif

    /* ---- NVS：WiFi 校准数据 + 我们的阈值配置都要用它 ---- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建 (%s)", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        /* NVS 挂了不致命：阈值配置退回默认值，WiFi 也能连 */
        ESP_LOGE(TAG, "NVS init failed: %s（阈值将无法保存）", esp_err_to_name(nvs_err));
    }

    /* ---- 硬件 ---- */
    led_status_set(LED_STATUS_BOOT);
    (void)board_init();   /* 返回 ESP_ERR_NOT_FOUND 只是提示有配件没接，继续跑 */

    /* ---- 业务模型（必须在 BSP 之后，因为它会调 led/servo/fan） ---- */
    ESP_ERROR_CHECK(device_model_init());

    /* ★ device_model_init() 会把三路舵机归位到 0°，这会让它们重新带电、持续保持力矩
     *   （servo_init() 结束时本来是把它们 detach 掉的）。
     *   空载时舵机"憋着劲"会有轻微嗡嗡声并且一直耗电，所以这里再松一次劲。
     *   之后收到任何舵机命令时会自动重新 attach，不影响功能。 */
    (void)servo_detach(SERVO_CURTAIN);
    (void)servo_detach(SERVO_WINDOW);
    (void)servo_detach(SERVO_DOOR);
    ESP_LOGI(TAG, "servos detached (idle, no holding torque)");

    /* ---- 自动联动规则 ---- */
    ESP_ERROR_CHECK(automation_init());

    /* ---- 开机画面 ---- */
#if CONFIG_APP_OLED_ENABLE
    /* OLED 由 astra UI 任务渲染（启动后接管屏幕，splash 不再单独画） */
#endif

    /* ---- WiFi（非阻塞） ---- */
    led_status_set(LED_STATUS_WIFI_CONNECTING);
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败，将以离线模式运行");
        led_status_set(LED_STATUS_ERROR);
    }

    /* ★ 等 WiFi 拿到 IP 再启动 MQTT（最多 10 秒）。
     *   否则 esp-mqtt 会立刻去解析 broker 域名，此时 DNS 还没就绪，开机日志会出现：
     *       E esp-tls: couldn't get hostname for :broker.emqx.io: getaddrinfo() returns 202
     *       E transport_base: Failed to open a new connection: 32769
     *       E mqtt_client: Error transport connect
     *   虽然 esp-mqtt 之后会自己重连成功，但日志很难看、也会掩盖真正的问题。 */
    if (!wifi_is_connected()) {
        ESP_LOGI(TAG, "等待 WiFi 获取 IP（最多 10 秒）再启动 MQTT ...");
        wifi_wait_connected(10000);
    }
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WiFi 仍未连上，MQTT 会自行边连边重试");
    }

    /* ---- MQTT（非阻塞，内部自己重连） ---- */
    if (mqtt_app_start() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败，手机端控制不可用");
    } else {
        /* 设备状态一变就自动上报给 MQTT。★这个回调槽位被 MQTT 独占，
         * OLED 不走回调（由主循环定时刷新），所以不冲突。 */
        mqtt_app_bind_device_events();
    }

    /* ---- BLE（非阻塞，内部起 NimBLE 主机任务） ----
     * ★ 和 MQTT【并列】的第二条控制链路，不是替代：两者共用同一套上行 JSON
     *   （app_link 注册表转发）和同一套下行命令解析（app_cmd_handle_*）。
     *
     * ★ 这里【绝对不要】再调 device_register_cb()：
     *   那个回调槽位只保留最后一个，mqtt_app_bind_device_events() 已经占了它，
     *   再注册一次会把 MQTT 的状态上报顶掉（而且不会报错，只会静默失效）。
     *   BLE 不需要任何回调 —— 设备状态一变，mqtt_publish_state() 会通过
     *   app_link_broadcast() 同时发给 MQTT 和 BLE。
     *
     * 失败只告警：蓝牙起不来时 WiFi/MQTT/语音/按键/自动联动全都照常工作。 */
#if CONFIG_APP_BLE_ENABLE
    if (ble_app_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE 启动失败，手机蓝牙控制不可用（WiFi/MQTT 不受影响）");
    }
#endif

    /* ---- 传感器自动采样（需求 ②③ 的数据源） ---- */
    if (sensor_start_auto(CONFIG_APP_SENSOR_PERIOD_MS) != ESP_OK) {
        ESP_LOGW(TAG, "传感器自动采样启动失败");
    }

    /* ---- 语音：注册指令回调。模块没接也不影响 voice_inject_cmd 的注入路径 ---- */
    voice_register_cb(voice_on_cmd, NULL);

    /* ---- 按键（KEY2 数字键） ---- */
    key_register_cb(key_on_event, NULL);

    /* ---- 五位 AD 键盘（IO10）：事件回显（导航由 astra UI 瞬时查询驱动） ---- */
    adkey_register_cb(adkey_on_event, NULL);

    /* ---- OLED 菜单 UI（astra 框架 + u8g2 画布）：屏幕渲染与五键导航全权接管 ---- */
    astra_ui_start();

    /* ---- 常驻主循环 ---- */
    xTaskCreate(app_loop_task, "app_loop", 4096, NULL, 5, NULL);

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
    xTaskCreate(serial_console_task, "console", 4096, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "system ready. free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
    ESP_LOGI(TAG, "串口调试台已启动 —— 输入 help 查看命令");
#endif
    ESP_LOGI(TAG, "（app_main 返回，后续由各任务驱动）");
}
