// 程序主入口
// 初始化各个模块，启动主循环任务
// Copyright (C) 2023-2024, Xiaodong Wang <

// 初始化各个模块，启动主循环任务
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ESP-IDF 头文件
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 各个模块的头文件
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

// 设备模型 + 自动联动规则
#include "device_model.h"
#include "automation.h"
#include "wifi_sta.h"
#include "mqtt_app.h"
#include "ble_app.h"
#include "selftest.h"
#include "astra_glue.h"

// 把语音指令变成动作
#if CONFIG_APP_SERIAL_DEBUG_ENABLE
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#endif

// 主循环周期运行
static const char *TAG = "MAIN";


// 把语音指令变成动作
static void voice_on_cmd(voice_cmd_t cmd, void *user) {
    (void)user;
    // 导入控制源
    const ctrl_source_t src = SRC_VOICE;

    switch (cmd) {
    // 单个房间的灯
    // VOICE_CMD_LED_LIVING_ON/OFF  -> DEV_LED_LIVING
    case VOICE_CMD_LED_LIVING_ON:   device_set_power(DEV_LED_LIVING,  true,  src); break;
    case VOICE_CMD_LED_LIVING_OFF:  device_set_power(DEV_LED_LIVING,  false, src); break;
    
    // VOICE_CMD_LED_KITCHEN_ON/OFF  -> DEV_LED_KITCHEN
    case VOICE_CMD_LED_KITCHEN_ON:  device_set_power(DEV_LED_KITCHEN, true,  src); break;
    case VOICE_CMD_LED_KITCHEN_OFF: device_set_power(DEV_LED_KITCHEN, false, src); break;

    // VOICE_CMD_LED_BEDROOM_ON/OFF  -> DEV_LED_BEDROOM
    case VOICE_CMD_LED_BEDROOM_ON:  device_set_power(DEV_LED_BEDROOM, true,  src); break;
    case VOICE_CMD_LED_BEDROOM_OFF: device_set_power(DEV_LED_BEDROOM, false, src); break;

    // VOICE_CMD_LED_BATH_ON/OFF     -> DEV_LED_BATH
    case VOICE_CMD_LED_BATH_ON:     device_set_power(DEV_LED_BATH,    true,  src); break;
    case VOICE_CMD_LED_BATH_OFF:    device_set_power(DEV_LED_BATH,    false, src); break;

    // 全部灯一起开关
    case VOICE_CMD_LED_ALL_ON:
    case VOICE_CMD_LED_ALL_OFF: {
        bool on = (cmd == VOICE_CMD_LED_ALL_ON);

        // 全开只开灯和风扇
        // 房间灯
        device_set_power(DEV_LED_LIVING,  on, src);
        // 厨房灯
        device_set_power(DEV_LED_KITCHEN, on, src);
        // 卧室灯
        device_set_power(DEV_LED_BEDROOM, on, src);
        // 浴室灯
        device_set_power(DEV_LED_BATH,    on, src);
        break;
    }

    // 风扇开关
    case VOICE_CMD_FAN_ON:  device_set_power(DEV_FAN, true,  src); break;
    case VOICE_CMD_FAN_OFF: device_set_power(DEV_FAN, false, src); break;

    // 窗户开关
    case VOICE_CMD_WINDOW_OPEN:  device_set_power(DEV_WINDOW, true,  src); break;
    case VOICE_CMD_WINDOW_CLOSE: device_set_power(DEV_WINDOW, false, src); break;

    // 门开关
    case VOICE_CMD_DOOR_OPEN:  device_set_power(DEV_DOOR, true,  src); break;
    case VOICE_CMD_DOOR_CLOSE: device_set_power(DEV_DOOR, false, src); break;

    // 窗帘开关
    case VOICE_CMD_CURTAIN_OPEN:  device_set_power(DEV_CURTAIN, true,  src); break;
    case VOICE_CMD_CURTAIN_CLOSE: device_set_power(DEV_CURTAIN, false, src); break;

    // 问温度就播报
    case VOICE_CMD_QUERY_TEMP: {
        const sensor_data_t *d = sensor_get_last();
        if (d->valid_temp) {
            // 只播温度
            voice_speak_temp(d->temperature, -1.0f);
        } else {
            voice_speak("温度传感器未连接");
        }
        break;
    }
    case VOICE_CMD_QUERY_HUMI: {
        const sensor_data_t *d = sensor_get_last();
        if (d->valid_temp) {
            // 只播湿度
            voice_speak_temp(-100.0f, d->humidity);
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

    // 自动模式总开关
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
        // 啥也不干
        return;
    }

    // 本地操作也上报
    mqtt_publish_event(voice_cmd_name(cmd));
    voice_speak("好的");
}


// 五键的名字表
static const char *const s_adkey_names[ADKEY_NUM] = { "1", "2", "3", "4", "OK" };

// 五键事件只打日志
static void adkey_on_event(adkey_id_t id, adkey_event_t ev, void *user) {
    (void)user;
    static const char *const s_ev_names[] = { "DOWN", "UP", "CLICK", "LONG" };

    // 打按下那次的读数
    ESP_LOGI(TAG, "[ADKEY] %s %s (触发读数=%dmV, 当前=%dmV)",
             s_adkey_names[id], s_ev_names[ev], adkey_last_mv(), adkey_raw_mv());
}

// 按键切换自动和风扇
static void key_on_event(key_id_t id, key_event_t ev, void *user) {
    (void)user;

    // 自检时不响应按键
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


// 主循环周期干活
static void app_loop_task(void *arg) {
    (void)arg;
    // 周期跑自动联动规则 + 定时上报传感器数据
    const uint32_t tick_ms= 500;

    // 按周期上报传感器
    const uint32_t mqtt_sensor_period_ms = CONFIG_APP_MQTT_PUBLISH_SENSOR_MS;

    // 开机先发一次
    uint32_t mqtt_sensor_acc = mqtt_sensor_period_ms;   
    bool last_mqtt = false;

    for (;;) {
        // 跑自动联动规则
        automation_tick(sensor_get_last());

        // 定时把数据发出去
        mqtt_sensor_acc += tick_ms;
        if (mqtt_sensor_acc >= mqtt_sensor_period_ms) {
            mqtt_sensor_acc = 0;

            // MQTT连接了才发
            if (mqtt_is_connected()) {
                mqtt_publish_sensor(sensor_get_last());
            }
        }

        // 灯跟着网络状态变
        bool mqtt_now = mqtt_is_connected();
        if (mqtt_now != last_mqtt) {
            last_mqtt = mqtt_now;

            // MQTT连上了就亮青灯，没连上但WiFi连上了就亮绿灯
            if (mqtt_now) {
                led_status_set(LED_STATUS_MQTT_OK);
            } else if (wifi_is_connected()) {
                led_status_set(LED_STATUS_WIFI_OK);
            }
        }

        // 推着自检往前走
        if (selftest_is_active()) {
            selftest_tick();
        }

        // 屏幕交给界面层
        vTaskDelay(pdMS_TO_TICKS(tick_ms));
    }
}


// 串口调试台
#if CONFIG_APP_SERIAL_DEBUG_ENABLE

// 列出最吵的日志名
static const char *const s_noisy_tags[] = {
    "NimBLE",
    "ble_app",
    "BLE",
    "wifi",
    "net80211",
    "phy_init",
    "esp_netif_handlers",
    "spi_flash",
    "heap_init",
};
#define NOISY_TAG_CNT (sizeof(s_noisy_tags) / sizeof(s_noisy_tags[0]))

// 开机只压蓝牙日志
static const char *const s_ble_tags[] = { "NimBLE", "ble_app", "BLE" };
#define BLE_TAG_CNT (sizeof(s_ble_tags) / sizeof(s_ble_tags[0]))

// 批量设置日志等级
static void log_set_tags(const char *const *tags, size_t cnt, esp_log_level_t lv) {
    for (size_t i = 0; i < cnt; i++) {
        esp_log_level_set(tags[i], lv);
    }
}

// 把字符变成日志等级
static esp_log_level_t log_level_from_char(char c) {
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

// 把日志等级变成字符
static const char *log_level_name(esp_log_level_t lv) {
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

// 打印串口调试台帮助
static void console_print_help(void) {
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

// 扫五键接在哪
static void console_keyscan(int seconds)
{
    // 只测 IO10
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

        // 变了才打印
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

// 看麦克风有没有声
static void console_mic_level(void)
{
    // 先读一小段再打印
    // 采一小段音频
    int16_t probe[320] = { 0 };
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

// 把 all 展开成动作
static void console_dev_action(const char *dev, const char *action, int value)
{
    if (strcmp(dev, "all") == 0) {
        if (strcmp(action, "off") == 0) {
            device_all_off(SRC_MQTT);
        } else if (strcmp(action, "on") == 0) {
            // 全开只开灯和风扇
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
    // 去掉行尾回车换行
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

    // 数字统一取出来
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
        // 假装说了一句话
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
        // 插上就能测麦
        if (n >= 2 && strcmp(a1, "dump") == 0) {
            int secs = (n >= 3) ? atoi(a2) : 3;
            i2s_mic_dump(secs);
        } else {
            console_mic_level();
        }
    } else if (strcmp(cmd, "voice-test") == 0) {
        // 打印能听懂的词
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
        // 进退和跑自检
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
            // 这些日志名也要改回
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

// 串口收命令
static void serial_console_task(void *arg)
{
    (void)arg;

    // 装个只收的串口
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
        // 退格键
        } else if (ch == 0x08 || ch == 0x7F) {
            if (pos > 0) {
                pos--;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}
#endif


// 开机启动整个系统
void app_main(void)
{
    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "###########################################################");
    ESP_LOGI(TAG, "#  ESP32-S3 智能家居  (ESP-IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "#  build %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "###########################################################");

#if CONFIG_APP_SERIAL_DEBUG_ENABLE
    // 开机先压住蓝牙日志
    log_set_tags(s_ble_tags, BLE_TAG_CNT, ESP_LOG_WARN);
#endif

    // 备好存配置的地方
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建 (%s)", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        // 存不了就用默认值
        ESP_LOGE(TAG, "NVS init failed: %s（阈值将无法保存）", esp_err_to_name(nvs_err));
    }

    // 把硬件准备好
    led_status_set(LED_STATUS_BOOT);
    // 缺配件也接着跑
    (void)board_init();

    // 建好设备状态表
    ESP_ERROR_CHECK(device_model_init());

    // 舵机松劲省电
    (void)servo_detach(SERVO_CURTAIN);
    (void)servo_detach(SERVO_WINDOW);
    (void)servo_detach(SERVO_DOOR);
    ESP_LOGI(TAG, "servos detached (idle, no holding torque)");

    // 启动自动联动
    ESP_ERROR_CHECK(automation_init());

    // 开机画面交给界面
#if CONFIG_APP_OLED_ENABLE
    // 屏幕由界面层接管
#endif

    // 连上路由器
    led_status_set(LED_STATUS_WIFI_CONNECTING);
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败，将以离线模式运行");
        led_status_set(LED_STATUS_ERROR);
    }

    // 等联网成功再上云
    if (!wifi_is_connected()) {
        ESP_LOGI(TAG, "等待 WiFi 获取 IP（最多 10 秒）再启动 MQTT ...");
        wifi_wait_connected(10000);
    }
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WiFi 仍未连上，MQTT 会自行边连边重试");
    }

    // 连上云收命令
    if (mqtt_app_start() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败，手机端控制不可用");
    } else {
        // 状态一变就上报
        mqtt_app_bind_device_events();
    }

    // 再开一条蓝牙链路
#if CONFIG_APP_BLE_ENABLE
    if (ble_app_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE 启动失败，手机蓝牙控制不可用（WiFi/MQTT 不受影响）");
    }
#endif

    // 定时采传感器
    if (sensor_start_auto(CONFIG_APP_SENSOR_PERIOD_MS) != ESP_OK) {
        ESP_LOGW(TAG, "传感器自动采样启动失败");
    }

    // 接上语音指令
    voice_register_cb(voice_on_cmd, NULL);

    // 接上按键
    key_register_cb(key_on_event, NULL);

    // 接上五键键盘
    adkey_register_cb(adkey_on_event, NULL);

    // 起界面管屏幕
    astra_ui_start();

    // 起主循环
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
