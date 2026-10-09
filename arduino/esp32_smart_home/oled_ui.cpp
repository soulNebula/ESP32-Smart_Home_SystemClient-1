// ======================================================================
// 屏幕：SSD1306 128x64 走 U8g2，画主页 / 设备页 / 传感器页 / 自检页 / 联网页
//
// 对应 ESP-IDF 工程的 components/BSP/OLED/oled.c
// 加 main/astra_glue.cpp 里那几页显示什么、中文标签怎么写、多久刷一次。
// 原版那套自绘 GUI 框架这边不用，直接拿 U8g2 画同样的几行字。
//
// 中文字库：u8g2_font_wqy12_t_gb2312（文泉驿十二点阵，全字库，吃两百来K Flash）
//   原工程 components/astra_ui 里 mainFont 用的就是这个，画中文走 drawUTF8
//   想省地方换 u8g2_font_wqy12_t_chinese1（只带常用字，九K左右）
//   想大一点换 u8g2_font_wqy13_t_gb2312，行距那些一起改
//   数字想更大就单开一行用 u8g2_font_10x20_mn（纯 ASCII，原版主页就是这么放温度的）
//
// 五键映射（和原工程 main/astra_glue.cpp 里那份 KEY_MAP 一样）：
//   左 ADKEY_1（1397mV） 右 ADKEY_2（2639mV） 翻页
//   上 ADKEY_3（627mV）  下 ADKEY_4（1971mV） 选设备
//   OK ADKEY_OK（低压档） 确认
//   本文件只管画面，按键在 input 那边按这个表调过来
// ======================================================================

#include "oled_ui.h"

#include "app_config.h"

// Serial 两边都要用（没装库那一支也可能打日志），所以放在开关外头
#include <Arduino.h>

#if APP_OLED_ENABLE

#include <U8g2lib.h>
#include <Wire.h>

#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "board_config.h"
#include "device_model.h"
#include "net_ble.h"
#include "net_mqtt.h"
#include "selftest.h"
#include "sensor.h"

// 日志前缀，和原版 ESP-IDF 的 TAG 对齐
static const char *TAG = "OLED";

// 中文点阵字库，换字号记得把 OLED_LINE_H 和 OLED_ROW_STEP 一起改
#define OLED_FONT_CN        u8g2_font_wqy12_t_gb2312
// 这一档字连下沉一共占多高
#define OLED_LINE_H         13
// 顶上标题条占多高
#define OLED_BAR_H          14
// 一页摆几行
#define OLED_ROWS           4
// 内容第一行的基线
#define OLED_ROW0_Y         25
// 行距，四行正好铺满
#define OLED_ROW_STEP       12
// 第 n 行的基线
#define OLED_ROW_Y(n)       (OLED_ROW0_Y + (n) * OLED_ROW_STEP)
// 提示条亮两秒
#define OLED_TOAST_MS       2000
// 提示条最多存这么多字节，屏幕放不下会再掐
#define OLED_TOAST_MAX      48

// 屏幕对象，构造函数就按 U8g2 的写法来，地址探到了再改
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// 屏幕能不能用
static bool         s_ready         = false;
// 现在第几页
static oled_page_t  s_page          = OLED_PAGE_HOME;
// 页面上高亮第几项
static uint8_t      s_cursor        = 0;
// 上一拍什么时候刷的
static uint32_t     s_last_draw_ms  = 0;
// 提示条什么时候收
static uint32_t     s_toast_until_ms = 0;
// 提示条上那行字
static char         s_toast[OLED_TOAST_MAX] = { 0 };
// 上一拍自检开着没，就为了进出的时候跟着换页
static bool         s_selftest_on   = false;

// 设备页的八台，顺序和原工程 astra_glue.cpp 里那份一样
static const device_id_t s_dev_ids[DEV_COUNT] = {
    DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH,
    DEV_FAN, DEV_WINDOW, DEV_DOOR, DEV_CURTAIN,
};

// 屏幕上的短名字；窗帘舵机在板级配置里写死停用，照实写
static const char *const s_dev_cn[DEV_COUNT] = {
    "客厅灯", "厨房灯", "卧室灯", "浴室灯", "风扇", "窗户", "门",
#if BSP_SERVO_CURTAIN_ENABLE
    "窗帘",
#else
    "窗帘(停用)",
#endif
};

// 探一下这个地址上有没有人应
static bool oled_probe_addr(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// 量这行字多宽，居中要用
static int oled_text_width(const char *s) {
    return (int)u8g2.getUTF8Width(s);
}

// 拿读数，拿不到给一份空的，后面就不用到处判空
static const sensor_data_t *oled_sensor(void) {
    static const sensor_data_t empty = {};
    const sensor_data_t *s = sensor_get_last();
    return (s != NULL) ? s : &empty;
}

// 温度拼成字，用整数拼：省得精简版 printf 打不出 %f
static void oled_fmt_temp(char *buf, size_t len, float t) {
    int t10 = (int)(t * 10.0f + (t >= 0.0f ? 0.5f : -0.5f));
    int frac = t10 % 10;
    if (frac < 0) {
        frac = -frac;
    }
    snprintf(buf, len, "%d.%dC", t10 / 10, frac);
}

// 有开合度的就那三台，灯不掺和
static bool oled_has_level(device_id_t id) {
    return (id == DEV_FAN) || (id == DEV_WINDOW) || (id == DEV_DOOR);
}

// 光标走到哪窗口跟到哪，别让它跑出屏
static uint8_t oled_window_first(uint8_t focus, uint8_t count) {
    if (count <= OLED_ROWS) {
        return 0;
    }
    uint8_t first = 0;
    if (focus >= OLED_ROWS) {
        first = (uint8_t)(focus - OLED_ROWS + 1);
    }
    // 尾巴上不够一行就往回退
    if ((uint16_t)first + OLED_ROWS > (uint16_t)count) {
        first = (uint8_t)(count - OLED_ROWS);
    }
    return first;
}

// 自检这一项走到哪一步了
static const char *oled_phase_str(uint8_t phase) {
    switch (phase) {
        case SELFTEST_PHASE_RUN:  return "检测中";
        case SELFTEST_PHASE_DONE: return "完成";
        // 还没跑就是等着按
        default:                  return "等待";
    }
}

// WiFi 那句，原版写的就是 WiFi OK / WiFi 断开
static const char *oled_wifi_str(void) {
#if APP_MQTT_ENABLE
    return net_mqtt_wifi_is_connected() ? "WiFi OK" : "WiFi 断开";
#else
    // 联网整个没编进来，别装成断线
    return "WiFi 关";
#endif
}

// MQTT 那句
static const char *oled_mqtt_str(void) {
#if APP_MQTT_ENABLE
    return net_mqtt_is_connected() ? "MQTT OK" : "MQTT 断开";
#else
    return "MQTT 关";
#endif
}

// 蓝牙那句
static const char *oled_ble_str(void) {
#if APP_BLE_ENABLE
    return net_ble_is_connected() ? "蓝牙 已连" : "蓝牙 未连";
#else
    return "蓝牙 关";
#endif
}

// 本机 IP，没连上模块会给 0.0.0.0
static const char *oled_ip_str(void) {
#if APP_MQTT_ENABLE
    return net_mqtt_ip_str();
#else
    return "--";
#endif
}

// 居中画一行中文
static void oled_draw_center(int y, const char *s) {
    int x = (OLED_WIDTH - oled_text_width(s)) / 2;
    if (x < 0) {
        // 字太长就顶格画，右边让它自己截
        x = 0;
    }
    u8g2.drawUTF8((u8g2_uint_t)x, (u8g2_uint_t)y, s);
}

// 顶上那条黑底白字，左边页名右边补一句，没有就给 NULL
static void oled_draw_bar(const char *left, const char *right) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, OLED_WIDTH, OLED_BAR_H);
    // 白底上写黑字
    u8g2.setDrawColor(0);
    if (left != NULL) {
        u8g2.drawUTF8(2, OLED_BAR_H - 3, left);
    }
    if (right != NULL) {
        int x = OLED_WIDTH - 2 - oled_text_width(right);
        if (x < 0) {
            x = 0;
        }
        u8g2.drawUTF8((u8g2_uint_t)x, OLED_BAR_H - 3, right);
    }
    u8g2.setDrawColor(1);
}

// 画一行，选中那行反显，块比字略高一点看着才齐
static void oled_draw_row(int y, const char *text, bool selected) {
    if (selected) {
        u8g2.setDrawColor(1);
        u8g2.drawBox(0, (u8g2_uint_t)(y - (OLED_LINE_H - 2)), OLED_WIDTH, OLED_LINE_H);
        // 反显块里写黑字
        u8g2.setDrawColor(0);
    }
    u8g2.drawUTF8(2, (u8g2_uint_t)y, text);
    u8g2.setDrawColor(1);
}

// 主页：温湿度、光照雨滴、联网三行，联动开关挂标题条右边
static void oled_draw_home(void) {
    char line[48];
    char num[16];
    const sensor_data_t *s = oled_sensor();

    // 联动开关放标题条上，原版就摆在那儿
    snprintf(line, sizeof(line), "自动%s", automation_is_enabled() ? "开" : "关");
    oled_draw_bar("智能家居", line);

    // 温湿度，芯片没认出来就打杠，跟原版一样
    if (s->valid_temp) {
        oled_fmt_temp(num, sizeof(num), s->temperature);
        snprintf(line, sizeof(line), "温度 %s 湿度 %d%%", num, (int)(s->humidity + 0.5f));
    } else {
        snprintf(line, sizeof(line), "温度 --.-C 湿度 --%%");
    }
    oled_draw_center(OLED_ROW_Y(0), line);

    // 光照和雨滴都报百分比
    snprintf(line, sizeof(line), "光照 %d%% 雨滴 %d%%",
             (int)(s->light_pct + 0.5f), (int)(s->rain_pct + 0.5f));
    oled_draw_center(OLED_ROW_Y(1), line);

    // 联网一行报两句，和原版的 netStr 一个意思
    snprintf(line, sizeof(line), "%s %s", oled_wifi_str(), oled_mqtt_str());
    oled_draw_center(OLED_ROW_Y(2), line);
}

// 设备页：八台一台一行，一屏四行，光标走到哪窗口跟到哪
static void oled_draw_device(void) {
    char line[48];
    char num[16];
    const uint8_t first = oled_window_first(s_cursor, (uint8_t)DEV_COUNT);

    // 标题条右边报个位置，翻到第几台心里有数
    snprintf(num, sizeof(num), "%u/%u", (unsigned)(s_cursor + 1), (unsigned)DEV_COUNT);
    oled_draw_bar("设备", num);

    for (uint8_t r = 0; r < OLED_ROWS; r++) {
        const uint8_t i = (uint8_t)(first + r);
        if (i >= (uint8_t)DEV_COUNT) {
            break;
        }
        const device_id_t id = s_dev_ids[i];
        const bool on = device_get_power(id);
        // 灯只有开关，风扇窗户门还有开合度，能写就一起写出来
        if (on && oled_has_level(id)) {
            snprintf(line, sizeof(line), "%s [开] %d%%", s_dev_cn[i], (int)device_get_level(id));
        } else {
            snprintf(line, sizeof(line), "%s [%s]", s_dev_cn[i], on ? "开" : "关");
        }
        oled_draw_row(OLED_ROW_Y(r), line, (i == s_cursor));
    }
}

// 传感器页：读数一行一样，原始毫伏也摆出来
static void oled_draw_sensor(void) {
    char line[48];
    char num[16];
    const sensor_data_t *s = oled_sensor();

    oled_draw_bar("传感器", NULL);

    // 温湿度，芯片不在就打杠
    if (s->valid_temp) {
        oled_fmt_temp(num, sizeof(num), s->temperature);
        snprintf(line, sizeof(line), "温度 %s 湿度 %d%%", num, (int)(s->humidity + 0.5f));
    } else {
        snprintf(line, sizeof(line), "温度 --.-C 湿度 --%%");
    }
    u8g2.drawUTF8(2, OLED_ROW_Y(0), line);

    // 光照百分比加光敏电阻的毫伏
    snprintf(line, sizeof(line), "光照 %d%% 光敏 %dmV", (int)(s->light_pct + 0.5f), s->light_mv);
    u8g2.drawUTF8(2, OLED_ROW_Y(1), line);

    // 雨滴百分比加雨滴那路的毫伏
    snprintf(line, sizeof(line), "雨滴 %d%% 雨量 %dmV", (int)(s->rain_pct + 0.5f), s->rain_mv);
    u8g2.drawUTF8(2, OLED_ROW_Y(2), line);

    // 下没下雨，采样时刻按秒报：毫秒那串太长塞不下
    snprintf(line, sizeof(line), "下雨 %s 采样 %lus",
             s->rain_detected ? "是" : "否", (unsigned long)(s->timestamp_ms / 1000UL));
    u8g2.drawUTF8(2, OLED_ROW_Y(3), line);
}

// 自检页：项名一列，正在测的那项后头接状态，光标那行反显
static void oled_draw_selftest(void) {
    char line[48];
    char num[16];
    const uint8_t cnt = selftest_get_count();
    const uint8_t cur = selftest_get_index();
    const uint8_t ph  = selftest_get_phase();

    snprintf(num, sizeof(num), "%u/%u",
             (unsigned)((cnt == 0) ? 0 : (cur + 1)), (unsigned)cnt);
    oled_draw_bar("自检", num);

    if (cnt == 0) {
        // 一项都没有就只剩标题条
        return;
    }

    // 跑起来了就盯着正在测的那项，没跑就跟着光标
    const uint8_t focus = selftest_is_active() ? cur : s_cursor;
    const uint8_t first = oled_window_first(focus, cnt);

    for (uint8_t r = 0; r < OLED_ROWS; r++) {
        const uint8_t i = (uint8_t)(first + r);
        if (i >= cnt) {
            break;
        }
        // 正在测的那项把进度接在名字后头，原版就是这么做的
        if (selftest_is_active() && i == cur) {
            snprintf(line, sizeof(line), "%s %s", selftest_get_item_name(i), oled_phase_str(ph));
        } else {
            snprintf(line, sizeof(line), "%s", selftest_get_item_name(i));
        }
        oled_draw_row(OLED_ROW_Y(r), line, (i == s_cursor));
    }
}

// 联网页：WiFi / MQTT / 蓝牙 / IP 各一行
static void oled_draw_net(void) {
    char line[40];

    oled_draw_bar("联网", NULL);

    u8g2.drawUTF8(2, OLED_ROW_Y(0), oled_wifi_str());
    u8g2.drawUTF8(2, OLED_ROW_Y(1), oled_mqtt_str());
    u8g2.drawUTF8(2, OLED_ROW_Y(2), oled_ble_str());
    snprintf(line, sizeof(line), "IP %s", oled_ip_str());
    u8g2.drawUTF8(2, OLED_ROW_Y(3), line);
}

// 提示条：黑底白字压在最上面，谁也别想盖住它
static void oled_draw_toast(void) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, OLED_WIDTH, OLED_BAR_H);
    u8g2.setDrawColor(0);
    oled_draw_center(OLED_BAR_H - 3, s_toast);
    u8g2.setDrawColor(1);
}

// 把屏幕准备好
bool oled_ui_init(void) {
    // 已经好了就直接返回，和原版一样是幂等的
    if (s_ready) {
        return true;
    }

    // 屏幕和温湿度共用一条总线，谁先起来谁 begin，重复 begin 无害
    Wire.begin(BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ);

    // 两种地址都试一下，有的板子焊的是 0x3D
    static const uint8_t candidates[] = { BSP_I2C_ADDR_SSD1306, 0x3D };
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (oled_probe_addr(candidates[i])) {
            addr = candidates[i];
            break;
        }
    }
    if (addr == 0) {
        Serial.printf("%s: SSD1306 not found at 0x%02X or 0x3D, OLED disabled\n",
                      TAG, BSP_I2C_ADDR_SSD1306);
        s_ready = false;
        return false;
    }
    if (addr != BSP_I2C_ADDR_SSD1306) {
        // U8g2 里的地址是八位的，探到的七位要左移一位
        u8g2.setI2CAddress((uint8_t)(addr << 1));
        Serial.printf("%s: SSD1306 found at 0x%02X (default is 0x%02X) - using 0x%02X\n",
                      TAG, addr, BSP_I2C_ADDR_SSD1306, addr);
    }

    // 板级表写的是 400k，U8g2 每次发之前会照着这个值拨 Wire 的时钟
    u8g2.setBusClock(BSP_I2C_FREQ_HZ);
    // 注意：U8g2 起显示的时候自己会 Wire.begin() 一次，S3 的默认脚就是 8/9，
    // 跟板级表一致，所以这里不冲突；换了脚就得把构造里的脚也填上
    u8g2.begin();
    // 先挑好字库，不然量字宽的接口会踩到空指针
    u8g2.setFont(OLED_FONT_CN);
    // 先清一遍显存再推上去，省得开机一小会儿满屏雪花
    u8g2.clearBuffer();
    u8g2.sendBuffer();

    s_ready          = true;
    s_page           = OLED_PAGE_HOME;
    s_cursor         = 0;
    s_toast_until_ms = 0;
    s_toast[0]       = '\0';
    s_selftest_on    = false;
    // 减一个周期，下一拍马上就刷，别让屏幕空着
    s_last_draw_ms   = millis() - BSP_OLED_PERIOD_MS;

    Serial.printf("%s: SSD1306 128x64 ready @0x%02X, %dkHz SDA=%d SCL=%d\n",
                  TAG, addr, (int)(BSP_I2C_FREQ_HZ / 1000),
                  (int)BSP_I2C_SDA_GPIO, (int)BSP_I2C_SCL_GPIO);
    return true;
}

// 屏幕能用吗
bool oled_ui_is_ready(void) {
    return s_ready;
}

// 主循环喊这个，到点才刷
void oled_ui_poll(void) {
    if (!s_ready) {
        return;
    }

    // 自检进来自检出去跟着换页：这块屏幕就是块提示牌
    const bool st_on = selftest_is_active();
    if (st_on != s_selftest_on) {
        s_selftest_on = st_on;
        oled_ui_set_page(st_on ? OLED_PAGE_SELFTEST : OLED_PAGE_HOME);
    }

    const uint32_t now = millis();

    // 提示条到点了，下一拍把它擦掉
    if (s_toast_until_ms != 0 && (int32_t)(now - s_toast_until_ms) >= 0) {
        s_toast_until_ms = 0;
    }

    // 到点才刷，别刷太勤
    if ((uint32_t)(now - s_last_draw_ms) < BSP_OLED_PERIOD_MS) {
        return;
    }
    s_last_draw_ms = now;

    u8g2.clearBuffer();
    u8g2.setFont(OLED_FONT_CN);
    u8g2.setDrawColor(1);

    switch (s_page) {
        case OLED_PAGE_HOME:
            oled_draw_home();
            break;
        case OLED_PAGE_DEVICE:
            oled_draw_device();
            break;
        case OLED_PAGE_SENSOR:
            oled_draw_sensor();
            break;
        case OLED_PAGE_SELFTEST:
            oled_draw_selftest();
            break;
        case OLED_PAGE_NET:
            oled_draw_net();
            break;
        default:
            break;
    }

    // 提示条最后画，压在整页上面
    if (s_toast_until_ms != 0) {
        oled_draw_toast();
    }

    u8g2.sendBuffer();
}

// 翻页
void oled_ui_next_page(void) {
    // 到尾页绕回第一页
    oled_ui_set_page((oled_page_t)((s_page + 1) % OLED_PAGE_MAX));
}

void oled_ui_prev_page(void) {
    // 第一页往前绕到最后一页
    oled_ui_set_page((oled_page_t)((s_page + OLED_PAGE_MAX - 1) % OLED_PAGE_MAX));
}

void oled_ui_set_page(oled_page_t page) {
    if (page >= OLED_PAGE_MAX) {
        return;
    }
    if (s_page != page) {
        s_page = page;
        // 换页了光标从头来，省得停在上页的位子上
        s_cursor = 0;
        Serial.printf("%s: page -> %s\n", TAG, oled_ui_page_name(page));
    }
}

oled_page_t oled_ui_get_page(void) {
    return s_page;
}

// 页面上高亮第几项，越界自己夹住
void oled_ui_set_cursor(uint8_t idx) {
    const uint8_t max = oled_ui_cursor_max(s_page);
    if (max == 0) {
        // 这页没得选
        s_cursor = 0;
    } else if (idx >= max) {
        s_cursor = (uint8_t)(max - 1);
    } else {
        s_cursor = idx;
    }
}

uint8_t oled_ui_get_cursor(void) {
    return s_cursor;
}

// 这页一共几项，没有可选项的页给 0
uint8_t oled_ui_cursor_max(oled_page_t page) {
    switch (page) {
        case OLED_PAGE_DEVICE:
            // 八台设备一台一项
            return (uint8_t)DEV_COUNT;
        case OLED_PAGE_SELFTEST:
            // 项数问自检模块要，别写死
            return selftest_get_count();
        default:
            return 0;
    }
}

// 页名，日志里看
const char *oled_ui_page_name(oled_page_t page) {
    switch (page) {
        case OLED_PAGE_HOME:     return "主页";
        case OLED_PAGE_DEVICE:   return "设备页";
        case OLED_PAGE_SENSOR:   return "传感器页";
        case OLED_PAGE_SELFTEST: return "自检页";
        case OLED_PAGE_NET:      return "联网页";
        default:                 return "未知";
    }
}

// 顶一行字闪一下，两秒后自己消失
void oled_ui_toast(const char *msg) {
    if (msg == NULL) {
        return;
    }

    // 先存一份，屏幕就十六来个字宽，装不下再掐尾巴
    strncpy(s_toast, msg, sizeof(s_toast) - 1);
    s_toast[sizeof(s_toast) - 1] = '\0';
    if (s_ready) {
        while (s_toast[0] != '\0' && oled_text_width(s_toast) > (OLED_WIDTH - 4)) {
            // 一个字符一个字符往回退，中文是三字节，别退一半留个乱码
            size_t n = strlen(s_toast);
            while (n > 0 && ((unsigned char)s_toast[n - 1] & 0xC0) == 0x80) {
                n--;
            }
            if (n == 0) {
                break;
            }
            s_toast[n - 1] = '\0';
        }
    }

    s_toast_until_ms = millis() + OLED_TOAST_MS;
    Serial.printf("%s: toast %s\n", TAG, s_toast);
}

#else

// ----------------------------------------------------------------------
// 屏幕开关关掉了：接口都留着，全给空实现
// 这样不装 U8g2 库也能编译，上层不用到处写 #if
// ----------------------------------------------------------------------

bool oled_ui_init(void) {
    return false;
}

bool oled_ui_is_ready(void) {
    return false;
}

void oled_ui_poll(void) {
}

void oled_ui_next_page(void) {
}

void oled_ui_prev_page(void) {
}

void oled_ui_set_page(oled_page_t page) {
    (void)page;
}

oled_page_t oled_ui_get_page(void) {
    return OLED_PAGE_HOME;
}

void oled_ui_set_cursor(uint8_t idx) {
    (void)idx;
}

uint8_t oled_ui_get_cursor(void) {
    return 0;
}

uint8_t oled_ui_cursor_max(oled_page_t page) {
    (void)page;
    return 0;
}

const char *oled_ui_page_name(oled_page_t page) {
    (void)page;
    // 没屏幕就没页名，给空串让日志照打
    return "";
}

void oled_ui_toast(const char *msg) {
    (void)msg;
}

#endif
