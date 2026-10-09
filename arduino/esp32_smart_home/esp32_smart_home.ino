// ======================================================================
// ESP32-S3 智能家居 · Arduino 版
//
// 这份是从 ESP-IDF 工程移植过来的：灯、窗帘、风扇、传感器、语音、
// 手机控制和自动联动都在，接线和手机 App 都不用改。
//
// 引脚和参数看 board_config.h，功能开关和 WiFi 账号看 app_config.h。
// 编译烧录步骤看上一级目录的 README.md。
//
// 和 ESP-IDF 版的区别（就这几条）：
//   1) 语音走 ASRPRO 串口模块；ESP-IDF 那套 INMP441 + ESP-SR 离线识别
//      是乐鑫的闭源库，Arduino 里没有，所以 GPIO13/39/40 那几个麦克风脚空着；
//   2) 屏幕是自己用 U8g2 画的五页，不是原来的 astra GUI 框架；
//   3) 窗帘舵机照样写死停用（BSP_SERVO_CURTAIN_ENABLE = 0）。
// ======================================================================

#include "app_config.h"
#include "board_config.h"

#include <stdlib.h>
#include <string.h>

#include "device_model.h"
#include "automation.h"
#include "sensor.h"
#include "led.h"
#include "fan.h"
#include "servo.h"
#include "input.h"
#include "voice.h"
#include "oled_ui.h"
#include "app_cmd.h"
#include "net_mqtt.h"
#include "net_ble.h"
#include "selftest.h"

// 命令台一行最长多少
#define CMD_LINE_MAX        128
// 一行最多切几个词
#define CMD_ARG_MAX         6

// 传感器多久往手机推一次
#define SENSOR_PUBLISH_MS   5000

// 状态灯多久看一次
#define STATUS_LED_MS       500

// 命令台收的行
static char    s_line[CMD_LINE_MAX];
static uint8_t s_line_len = 0;

// 页面光标，0 起
static uint8_t s_cursor = 0;

// 键盘探测：到点就停，不阻塞主循环
static uint32_t s_keyscan_until = 0;
static int      s_keyscan_last  = -1;

// 上次上报和上次看灯的时刻
static uint32_t s_publish_ms = 0;
static uint32_t s_status_ms  = 0;

// 上一次的联网状态，变了才动状态灯
static bool s_last_mqtt = false;
static bool s_last_wifi = false;

// 打印引脚表，照 board.c 那份
static void print_pinmap(void) {
    Serial.println("----------------- PIN MAP (source: board_config.h) -----------------");
    Serial.printf("  I2C    SDA=%d SCL=%d          -> OLED / AHT20\n",
                  BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
    Serial.printf("  ADC    LIGHT=%d RAIN=%d        -> 光敏 / 雨滴AO\n",
                  BSP_GPIO_LIGHT_ADC, BSP_GPIO_RAIN_AO);
    Serial.printf("  LED    LIVING=%d KITCHEN=%d BEDROOM=%d BATH=%d\n",
                  BSP_LED_GPIO_LIVING, BSP_LED_GPIO_KITCHEN,
                  BSP_LED_GPIO_BEDROOM, BSP_LED_GPIO_BATH);
#if BSP_SERVO_CURTAIN_ENABLE
    Serial.printf("  SERVO  CURTAIN=%d WINDOW=%d DOOR=%d\n",
                  BSP_SERVO_GPIO_CURTAIN, BSP_SERVO_GPIO_WINDOW, BSP_SERVO_GPIO_DOOR);
#else
    // 窗帘那路写死停用，别在引脚表里报它
    Serial.printf("  SERVO  CURTAIN=停用 WINDOW=%d DOOR=%d\n",
                  BSP_SERVO_GPIO_WINDOW, BSP_SERVO_GPIO_DOOR);
#endif
    Serial.printf("  FAN    PWM=%d\n", BSP_FAN_GPIO_PWM);
    Serial.printf("  KEY    KEY2=%d    ADKEY=%d\n", BSP_KEY_GPIO_KEY2, BSP_ADKEY_GPIO);
    Serial.printf("  VOICE  RX=%d TX=%d @%d\n",
                  BSP_VOICE_UART_RX_GPIO, BSP_VOICE_UART_TX_GPIO, BSP_VOICE_UART_BAUD);
    Serial.println("-------------------------------------------------------------------");
}

// 哪个功能开着、哪个因为没装库跳过了
static void print_features(void) {
    Serial.printf("功能开关: 屏幕=%s  MQTT=%s  蓝牙=%s  语音=%s  状态灯=%s\n",
                  APP_OLED_ENABLE ? "开" : "关（没装 U8g2 或手动关了）",
                  APP_MQTT_ENABLE ? "开" : "关（手动关了，或者没装 PubSubClient）",
                  APP_BLE_ENABLE  ? "开" : "关（手动关了，或者没装 NimBLE-Arduino）",
                  APP_VOICE_ENABLE ? "开" : "关",
                  APP_STATUS_LED_ENABLE ? "开" : "关");

#if !APP_OLED_ENABLE
    Serial.println("提示: 屏幕是关的。想用就在 管理库 里装 U8g2，再把 app_config.h 里 APP_OLED_ENABLE 改成 1");
#endif
#if !APP_MQTT_ENABLE
    Serial.println("提示: 联网是关的。想上云就在 管理库 里装 PubSubClient，再把 APP_MQTT_ENABLE 改成 1");
#endif
#if !APP_BLE_ENABLE
    Serial.println("提示: 蓝牙是关的。想用手机 App 直连就装 NimBLE-Arduino（核心包 2.x 装 1.4.x、3.x 装 2.x），再把 APP_BLE_ENABLE 改成 1");
#endif
}

// 命令台帮助
static void print_help(void) {
    Serial.println("  status                  查看全部设备 + 传感器状态");
    Serial.println("  on   <dev>              开设备  (led_living/led_kitchen/led_bedroom/led_bath/fan/window/door/curtain/all)");
    Serial.println("  off  <dev>              关设备");
    Serial.println("  toggle <dev>            翻转开关");
    Serial.println("  open  <dev> / close <dev>   开合窗户、门");
    Serial.println("  set  <dev> <0-100>      设备调档（灯亮度 / 风速 / 开合度）");
    Serial.println("  color <led_xxx> <r> <g> <b>  记一个颜色，单色灯只是记状态");
    Serial.println("  auto <on|off>           自动联动总开关");
    Serial.println("  cfg                     查看自动联动阈值");
    Serial.println("  cfg <key> <value>       改阈值");
    Serial.println("  say  <voice_cmd>        模拟走一遍语音链路，指令名见 help-voice");
    Serial.println("  help-voice              列出所有语音指令名");
    Serial.println("  adkey                   看五键键盘现在的电压");
    Serial.println("  keyscan <秒>            盯着键盘电压，按 1→2→3→4→OK 看哪个档变了");
    Serial.println("  test                    进出按键自检模式");
    Serial.println("  test run / next / off   自检：执行当前项 / 下一项 / 退出");
    Serial.println("  help                    帮助");
    Serial.println();
    Serial.println("  五键键盘：左(1)=上一页 右(2)=下一页 上(3)/下(4)=选项 OK=确认");
    Serial.println("  板载按键：KEY2 单击=切自动  长按=切风扇（本板 KEY1 的脚让给键盘了）");
}

// 命令台：语音指令名一览
static void print_voice_help(void) {
    Serial.println("  ---- 语音指令名（say 后面接这个）----");
    for (int i = 1; i < (int)VOICE_CMD_MAX; i++) {
        Serial.printf("  %-22s", voice_cmd_name((voice_cmd_t)i));
        if ((i % 4) == 0) {
            Serial.println();
        }
    }
    Serial.println();
}

// 命令台：全部状态
static void print_status(void) {
    Serial.println("  ---- 设备 ----");
    for (int i = 0; i < DEV_COUNT; i++) {
        Serial.printf("  %-12s power=%-3s level=%3d%%\n",
                      device_id_name((device_id_t)i),
                      device_get_power((device_id_t)i) ? "on" : "off",
                      (int)device_get_level((device_id_t)i));
    }

    char line[128];
    sensor_format_line(sensor_get_last(), line, sizeof(line));
    Serial.printf("  %s\n", line);

    Serial.printf("  自动联动=%s   屏幕=%s   蓝牙=%s\n",
                  automation_is_enabled() ? "开" : "关",
                  oled_ui_is_ready() ? "就绪" : "没接",
                  net_ble_is_connected() ? "已连" : "未连");

#if APP_MQTT_ENABLE
    Serial.printf("  WiFi=%s  IP=%s   MQTT=%s\n",
                  net_mqtt_wifi_is_connected() ? "已连" : "未连",
                  net_mqtt_ip_str(),
                  net_mqtt_is_connected() ? "已连" : "未连");
#endif
}

// 命令台：阈值
static void print_cfg(void) {
    char buf[256];
    automation_cfg_json(buf, sizeof(buf));
    Serial.printf("  %s\n", buf);
}

// 语音命令落到设备上，照 main.c 那张表
static void voice_cmd_apply(voice_cmd_t cmd, ctrl_source_t src) {
    switch (cmd) {
    // 单个房间的灯
    case VOICE_CMD_LED_LIVING_ON:   device_set_power(DEV_LED_LIVING,  true,  src); break;
    case VOICE_CMD_LED_LIVING_OFF:  device_set_power(DEV_LED_LIVING,  false, src); break;
    case VOICE_CMD_LED_KITCHEN_ON:  device_set_power(DEV_LED_KITCHEN, true,  src); break;
    case VOICE_CMD_LED_KITCHEN_OFF: device_set_power(DEV_LED_KITCHEN, false, src); break;
    case VOICE_CMD_LED_BEDROOM_ON:  device_set_power(DEV_LED_BEDROOM, true,  src); break;
    case VOICE_CMD_LED_BEDROOM_OFF: device_set_power(DEV_LED_BEDROOM, false, src); break;
    case VOICE_CMD_LED_BATH_ON:     device_set_power(DEV_LED_BATH,    true,  src); break;
    case VOICE_CMD_LED_BATH_OFF:    device_set_power(DEV_LED_BATH,    false, src); break;

    // 四路灯一起
    case VOICE_CMD_LED_ALL_ON:
    case VOICE_CMD_LED_ALL_OFF:
    {
        const bool on = (cmd == VOICE_CMD_LED_ALL_ON);
        device_set_power(DEV_LED_LIVING,  on, src);
        device_set_power(DEV_LED_KITCHEN, on, src);
        device_set_power(DEV_LED_BEDROOM, on, src);
        device_set_power(DEV_LED_BATH,    on, src);
        break;
    }

    // 风扇
    case VOICE_CMD_FAN_ON:  device_set_power(DEV_FAN, true,  src); break;
    case VOICE_CMD_FAN_OFF: device_set_power(DEV_FAN, false, src); break;

    // 窗户和门
    case VOICE_CMD_WINDOW_OPEN:  device_set_power(DEV_WINDOW, true,  src); break;
    case VOICE_CMD_WINDOW_CLOSE: device_set_power(DEV_WINDOW, false, src); break;
    case VOICE_CMD_DOOR_OPEN:    device_set_power(DEV_DOOR,   true,  src); break;
    case VOICE_CMD_DOOR_CLOSE:   device_set_power(DEV_DOOR,   false, src); break;

    // 窗帘，舵机停用了，命令会被上层拒掉
    case VOICE_CMD_CURTAIN_OPEN:  device_set_power(DEV_CURTAIN, true,  src); break;
    case VOICE_CMD_CURTAIN_CLOSE: device_set_power(DEV_CURTAIN, false, src); break;

    // 查询类：这版没喇叭，回了也听不见，只记日志
    case VOICE_CMD_QUERY_TEMP:
    case VOICE_CMD_QUERY_HUMI:
    case VOICE_CMD_QUERY_LIGHT:
    case VOICE_CMD_QUERY_ALL:
    case VOICE_CMD_QUERY_STATUS:
        break;

    // 自动模式总开关
    case VOICE_CMD_AUTO_ON:
        automation_set_enabled(true);
        automation_save();
        break;
    case VOICE_CMD_AUTO_OFF:
        automation_set_enabled(false);
        automation_save();
        break;

    case VOICE_CMD_NONE:
    default:
        return;
    }

    // 本地操作上报
    net_mqtt_publish_event(voice_cmd_name(cmd));
}

// 认出语音命令了：先打日志、屏幕上闪一下，再落到设备
static void voice_on_cmd(voice_cmd_t cmd, void *user_data) {
    (void)user_data;

    Serial.printf("voice: 命令词命中 -> %s\n", voice_cmd_name(cmd));

#if APP_OLED_ENABLE
    char note[40];
    snprintf(note, sizeof(note), "语音: %s", voice_cmd_name(cmd));
    oled_ui_toast(note);
#endif

    voice_cmd_apply(cmd, SRC_VOICE);
}

// 板载按键：KEY2 单击切自动，长按切风扇
static void key_on_event(key_id_t id, key_event_t ev, void *user_data) {
    (void)user_data;

    // 自检时不响应按键
    if (selftest_is_active()) {
        return;
    }

    if (id == KEY_ID_1) {
        if (ev == KEY_EVENT_CLICK) {
            Serial.println("MAIN: [KEY1] click -> 切客厅灯");
            device_toggle(DEV_LED_LIVING, SRC_LOCAL_KEY);
            net_mqtt_publish_event("key1_click");
        } else if (ev == KEY_EVENT_LONG_PRESS) {
            Serial.println("MAIN: [KEY1] long  -> 全关");
            device_all_off(SRC_LOCAL_KEY);
            net_mqtt_publish_event("key1_long_all_off");
        }
    } else if (id == KEY_ID_2) {
        if (ev == KEY_EVENT_CLICK) {
            const bool now = !automation_is_enabled();
            Serial.printf("MAIN: [KEY2] click -> 自动联动 %s\n", now ? "开" : "关");
            automation_set_enabled(now);
            automation_save();
            net_mqtt_publish_event(now ? "key2_auto_on" : "key2_auto_off");
        } else if (ev == KEY_EVENT_LONG_PRESS) {
            Serial.println("MAIN: [KEY2] long  -> 切风扇");
            device_toggle(DEV_FAN, SRC_LOCAL_KEY);
            net_mqtt_publish_event("key2_long_fan_toggle");
        }
    }
}

// 把光标限制在这一页的项数里
static void cursor_clamp(void) {
    const uint8_t max = oled_ui_cursor_max(oled_ui_get_page());
    if (max == 0) {
        s_cursor = 0;
        return;
    }
    if (s_cursor >= max) {
        s_cursor = (uint8_t)(max - 1);
    }
    oled_ui_set_cursor(s_cursor);
}

// OK 键按下去干什么，看当前在哪一页
static void adkey_confirm(void) {
    if (selftest_is_active()) {
        // 自检模式下 OK 就是跑当前项
        selftest_run_current();
        return;
    }

    switch (oled_ui_get_page()) {
    case OLED_PAGE_DEVICE:
        // 设备页：切选中的那台
        device_toggle((device_id_t)s_cursor, SRC_LOCAL_KEY);
        break;

    case OLED_PAGE_SELFTEST:
        // 进自检模式
        selftest_enter();
        selftest_run_index(s_cursor);
        break;

    default:
        break;
    }
}

// 五键键盘：左右翻页、上下选项、OK 确认
static void adkey_on_event(adkey_id_t id, adkey_event_t ev, void *user_data) {
    (void)user_data;

    // 只认短按和长按，按下/抬起不用管
    if ((ev != ADKEY_EVENT_CLICK) && (ev != ADKEY_EVENT_LONG_PRESS)) {
        return;
    }

    static const char *const ev_names[] = { "DOWN", "UP", "CLICK", "LONG" };
    Serial.printf("MAIN: [ADKEY] %s %s (触发读数=%dmV, 当前=%dmV)\n",
                  (id == ADKEY_OK) ? "OK" : ((id == ADKEY_1) ? "1" : ((id == ADKEY_2) ? "2" :
                  ((id == ADKEY_3) ? "3" : "4"))),
                  ev_names[ev], adkey_last_mv(), adkey_raw_mv());

    // 自检模式下：3/4 切项，OK 长按退出
    if (selftest_is_active()) {
        if (id == ADKEY_3) {
            selftest_next();
        } else if (id == ADKEY_4) {
            selftest_next();
        } else if ((id == ADKEY_OK) && (ev == ADKEY_EVENT_LONG_PRESS)) {
            Serial.println("MAIN: 自检退出");
            selftest_exit();
        } else if ((id == ADKEY_OK) && (ev == ADKEY_EVENT_CLICK)) {
            selftest_run_current();
        }
        return;
    }

    switch (id) {
    // 左：上一页
    case ADKEY_1:
        if (ev == ADKEY_EVENT_CLICK) {
            oled_ui_prev_page();
            cursor_clamp();
        }
        break;

    // 右：下一页
    case ADKEY_2:
        if (ev == ADKEY_EVENT_CLICK) {
            oled_ui_next_page();
            cursor_clamp();
        }
        break;

    // 上：往上选
    case ADKEY_3:
        if (s_cursor > 0) {
            s_cursor--;
        }
        cursor_clamp();
        break;

    // 下：往下选
    case ADKEY_4: {
        const uint8_t max = oled_ui_cursor_max(oled_ui_get_page());
        if ((max > 0) && (s_cursor + 1 < max)) {
            s_cursor++;
        }
        cursor_clamp();
        break;
    }

    // OK：确认
    case ADKEY_OK:
        if (ev == ADKEY_EVENT_CLICK) {
            adkey_confirm();
        }
        break;

    default:
        break;
    }
}

// 设备一变就往上推，手机两路都发
static void device_on_change(device_id_t id, ctrl_source_t src, void *user_data) {
    (void)id;
    (void)src;
    (void)user_data;

    net_mqtt_publish_state();
    net_ble_notify_state();
}

// 键盘探测：打印电压变化，不阻塞
static void keyscan_poll(void) {
    if (s_keyscan_until == 0) {
        return;
    }

    const int mv = adkey_raw_mv();
    if (mv != s_keyscan_last) {
        s_keyscan_last = mv;
        Serial.printf("MAIN: [keyscan] IO%d=%4dmV\n", BSP_ADKEY_GPIO, mv);
    }

    if ((int32_t)(millis() - s_keyscan_until) >= 0) {
        s_keyscan_until = 0;
        Serial.printf("MAIN: [keyscan] 结束，最后读数 %dmV\n", mv);
    }
}

// 切一行命令，返回第几个词
static int split_args(char *line, char *argv[], int max_arg) {
    int n = 0;
    char *p = line;

    while ((*p != '\0') && (n < max_arg)) {
        // 跳掉空格
        while ((*p == ' ') || (*p == '\t')) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        argv[n++] = p;

        // 找这个词的结尾
        while ((*p != '\0') && (*p != ' ') && (*p != '\t')) {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }

    return n;
}

// 命令台：设备动作
static void cmd_device_action(const char *action, const char *dev_name) {
    const device_id_t id = device_from_name(dev_name);

    if (id == DEV_COUNT) {
        // all 只支持开关
        if (strcmp(dev_name, "all") != 0) {
            Serial.printf("MAIN: 未知设备: %s（输入 help 看列表）\n", dev_name);
            return;
        }

        if (strcmp(action, "on") == 0) {
            for (int i = 0; i < DEV_COUNT; i++) {
                device_set_power((device_id_t)i, true, SRC_MQTT);
            }
        } else if (strcmp(action, "off") == 0) {
            device_all_off(SRC_MQTT);
        } else {
            Serial.println("MAIN: all 只支持 on / off");
            return;
        }

        Serial.printf("MAIN: %s all -> OK\n", action);
        return;
    }

    bool ok = false;
    if (strcmp(action, "on") == 0) {
        ok = device_set_power(id, true, SRC_MQTT);
    } else if (strcmp(action, "off") == 0) {
        ok = device_set_power(id, false, SRC_MQTT);
    } else if (strcmp(action, "toggle") == 0) {
        ok = device_toggle(id, SRC_MQTT);
    } else if (strcmp(action, "open") == 0) {
        ok = device_set_power(id, true, SRC_MQTT);
    } else if (strcmp(action, "close") == 0) {
        ok = device_set_power(id, false, SRC_MQTT);
    } else {
        Serial.printf("MAIN: 未知动作: %s\n", action);
        return;
    }

    Serial.printf("MAIN: %s %s -> %s\n", action, dev_name, ok ? "OK" : "失败");
}

// 收完整一行，执行
static void run_command(char *line) {
    char *argv[CMD_ARG_MAX];
    const int n = split_args(line, argv, CMD_ARG_MAX);

    if (n == 0) {
        return;
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "help-voice") == 0) {
        print_voice_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if ((strcmp(cmd, "on") == 0) || (strcmp(cmd, "off") == 0) ||
               (strcmp(cmd, "toggle") == 0) || (strcmp(cmd, "open") == 0) ||
               (strcmp(cmd, "close") == 0)) {
        if (n < 2) {
            Serial.printf("MAIN: 用法: %s <dev>\n", cmd);
            return;
        }
        cmd_device_action(cmd, argv[1]);
    } else if (strcmp(cmd, "set") == 0) {
        if (n < 3) {
            Serial.println("MAIN: 用法: set <dev> <0-100>");
            return;
        }
        const device_id_t id = device_from_name(argv[1]);
        if (id >= DEV_COUNT) {
            Serial.printf("MAIN: 未知设备: %s\n", argv[1]);
            return;
        }
        const bool ok = device_set_level(id, (uint8_t)atoi(argv[2]), SRC_MQTT);
        Serial.printf("MAIN: set %s %s -> %s\n", argv[1], argv[2], ok ? "OK" : "失败");
    } else if (strcmp(cmd, "color") == 0) {
        if (n < 5) {
            Serial.println("MAIN: 用法: color <led_xxx> <r> <g> <b>");
            return;
        }
        const device_id_t id = device_from_name(argv[1]);
        if (id >= DEV_COUNT) {
            Serial.printf("MAIN: 未知设备: %s\n", argv[1]);
            return;
        }
        const bool ok = device_set_color(id, (uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]),
                                         (uint8_t)atoi(argv[4]), SRC_MQTT);
        Serial.printf("MAIN: color %s -> %s\n", argv[1], ok ? "OK" : "失败");
    } else if (strcmp(cmd, "auto") == 0) {
        const bool on = (n >= 2) && (strcmp(argv[1], "on") == 0);
        automation_set_enabled(on);
        automation_save();
        Serial.printf("MAIN: auto mode -> %s\n", on ? "ON" : "OFF");
    } else if (strcmp(cmd, "cfg") == 0) {
        if (n < 3) {
            print_cfg();
            return;
        }
        const bool ok = automation_set_threshold(argv[1], (float)atof(argv[2]));
        if (ok) {
            automation_save();
        }
        Serial.printf("MAIN: cfg %s = %s -> %s\n", argv[1], argv[2], ok ? "OK" : "失败");
    } else if (strcmp(cmd, "say") == 0) {
        if (n < 2) {
            Serial.println("MAIN: 用法: say <voice_cmd>");
            return;
        }
        const voice_cmd_t vc = voice_cmd_from_name(argv[1]);
        if (vc == VOICE_CMD_NONE) {
            Serial.printf("MAIN: 未知语音指令: %s（help-voice 看列表）\n", argv[1]);
            return;
        }
        voice_inject_cmd(vc);
    } else if (strcmp(cmd, "adkey") == 0) {
        Serial.printf("MAIN: [adkey] IO%d=%dmV 按下=%s\n",
                      BSP_ADKEY_GPIO, adkey_raw_mv(),
                      adkey_is_pressed(ADKEY_OK) ? "是" : "否");
    } else if (strcmp(cmd, "keyscan") == 0) {
        const int seconds = (n >= 2) ? atoi(argv[1]) : 10;
        s_keyscan_until = millis() + (uint32_t)((seconds > 0 ? seconds : 10) * 1000);
        s_keyscan_last  = -1;
        Serial.printf("MAIN: [keyscan] 盯 %d 秒：按 1→2→3→4→OK，电压一变就打印\n",
                      seconds > 0 ? seconds : 10);
    } else if (strcmp(cmd, "test") == 0) {
        if (n < 2) {
            if (selftest_is_active()) {
                selftest_exit();
                Serial.println("MAIN: selftest: OFF");
            } else {
                selftest_enter();
                Serial.println("MAIN: selftest: ON（3/4 切换 / OK 执行 / OK 长按退出）");
            }
            return;
        }

        if (strcmp(argv[1], "run") == 0) {
            selftest_run_current();
            Serial.println("MAIN: selftest: run current item");
        } else if (strcmp(argv[1], "next") == 0) {
            selftest_next();
            Serial.println("MAIN: selftest: next item");
        } else if (strcmp(argv[1], "off") == 0) {
            selftest_exit();
            Serial.println("MAIN: selftest: OFF");
        } else {
            Serial.println("MAIN: 用法: test [run|next|off]");
        }
    } else {
        Serial.printf("MAIN: 未知命令: %s（输入 help 看帮助）\n", cmd);
    }
}

// 串口收行，收满一行就执行
static void console_poll(void) {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) {
            return;
        }

        if ((c == '\r') || (c == '\n')) {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                run_command(s_line);
                s_line_len = 0;
            }
            continue;
        }

        if (s_line_len < (CMD_LINE_MAX - 1)) {
            s_line[s_line_len++] = (char)c;
        } else {
            // 太长了就丢掉重来
            s_line_len = 0;
            Serial.println("MAIN: 命令行太长，丢掉了");
        }
    }
}

// 状态灯跟着联网状态走
static void status_led_poll(void) {
    if ((uint32_t)(millis() - s_status_ms) < STATUS_LED_MS) {
        return;
    }
    s_status_ms = millis();

    const bool wifi_now = net_mqtt_wifi_is_connected();
    const bool mqtt_now = net_mqtt_is_connected();

    if ((mqtt_now == s_last_mqtt) && (wifi_now == s_last_wifi)) {
        return;
    }
    s_last_mqtt = mqtt_now;
    s_last_wifi = wifi_now;

    if (mqtt_now) {
        // 上云了亮青灯
        led_status_set(LED_STATUS_MQTT_OK);
    } else if (wifi_now) {
        // 只有网，亮绿灯
        led_status_set(LED_STATUS_WIFI_OK);
    } else {
        // 还没连上，黄灯慢闪
        led_status_set(LED_STATUS_WIFI_CONNECTING);
    }
}

void setup(void) {
    Serial.begin(APP_SERIAL_BAUD);
    delay(200);

    Serial.println();
    Serial.println("###########################################################");
    Serial.println("#  ESP32-S3 智能家居 · Arduino 版");
    Serial.printf("#  build %s %s\n", __DATE__, __TIME__);
    Serial.println("###########################################################");

    print_pinmap();
    print_features();

    // 板上那颗灯先蓝灯慢闪
    led_status_set(LED_STATUS_BOOT);

    // 建好设备状态表，里面会把灯、舵机、风扇都准备好
    device_model_init();

    // 传感器
    sensor_init();
    sensor_start_auto(BSP_SENSOR_PERIOD_MS);

    // 按键和五键键盘
    input_init();
    key_register_cb(key_on_event, NULL);
    adkey_register_cb(adkey_on_event, NULL);

    // 屏幕
    oled_ui_init();

    // 语音
    voice_init();
    voice_register_cb(voice_on_cmd, NULL);

    // 状态一变就往上推
    device_register_cb(device_on_change, NULL);

    // 阈值读回来，再跑联动
    automation_init();

    // 联网和蓝牙
    net_mqtt_init();
    net_ble_init();

    // 开机先把当前状态推一次
    net_mqtt_publish_state();
    net_ble_notify_state();

    Serial.println("MAIN: 串口调试台已就绪，输入 help 看命令");
}

void loop(void) {
    // 采样到点才真采，不阻塞
    sensor_poll();

    // 按键、键盘、语音都在这儿收
    input_poll();
    voice_poll();

    // 舵机到点松劲
    servo_poll();

    // 自动联动
    automation_tick(sensor_get_last());

    // 自检往前推
    if (selftest_is_active()) {
        selftest_tick();
    }

    // 联网收发
    net_mqtt_poll();
    net_ble_poll();

    // 屏幕和状态灯
    oled_ui_poll();
    led_poll();
    status_led_poll();

    // 定时把传感器数据推给手机
    if ((uint32_t)(millis() - s_publish_ms) >= SENSOR_PUBLISH_MS) {
        s_publish_ms = millis();
        net_mqtt_publish_sensor();
        net_ble_notify_sensor();
    }

    // 键盘探测
    keyscan_poll();

    // 串口命令台
    console_poll();

    // 让别的任务也有机会跑，1ms 够用
    delay(1);
}
