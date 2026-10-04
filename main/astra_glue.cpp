/**
 * @file astra_glue.cpp
 * @brief astra UI 框架与智能家居业务之间的胶水层（main 组件内，C++）。
 *        组件方向约束：astra_ui 不能反向依赖 main —— OLED 刷新/按键读取
 *        以回调注入 HAL，业务数据（传感器/设备/自检状态）每帧写进页面字段。
 *
 * 键位（astra 键序 {UP, DOWN, LEFT, RIGHT, OK} → 本机五键键盘）：
 *        本键盘实测布局        [3]
 *   [1]  [OK]  [2]    →  3=UP、4=DOWN、1=LEFT、2=RIGHT，无需旋转
 *         [4]
 */
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "oled.h"
#include "adkey.h"
#include "sensor.h"
#include "device_model.h"
#include "automation.h"
#include "wifi_sta.h"
#include "mqtt_app.h"
#include "selftest.h"
#include "voice_esp_sr.h"   /* 语音识别 → OLED 弹窗提示（无喇叭方案的"反馈音"） */
}

#include "astra_rocket.h"
#include "astra_glue.h"

static const char *TAG = "astra_glue";

/* astra 键序 {UP, DOWN, LEFT, RIGHT, OK} → 本机 adkey_id_t（见文件头键位图） */
static const adkey_id_t KEY_MAP[key::KEY_NUM] = { ADKEY_3, ADKEY_4, ADKEY_1, ADKEY_2, ADKEY_OK };

/* ---------- 桥接回调（注入 astra HAL） ---------- */

static void flush_page_cb(uint8_t page, const uint8_t *data)
{
    oled_write_page(page, data);
}

static bool key_down_cb(uint8_t idx)
{
    if (idx >= key::KEY_NUM) {
        return false;
    }
    return adkey_is_pressed(KEY_MAP[idx]);
}

static void beep_cb(float freq)
{
    (void)freq;   /* 本机无蜂鸣器 */
}

/* ---------- 页面 OK 回调（业务动作） ---------- */

static void selftest_ok_cb(int itemIndex)
{
    if (itemIndex >= 0 && itemIndex < (int)selftest_get_count()) {
        selftest_run_index((uint8_t)itemIndex);
    }
}

static void device_ok_cb(int itemIndex)
{
    /* 8 行顺序：4 灯 + 风扇 + 窗/门/窗帘 */
    static const device_id_t devs[UI_DEVICE_ITEMS] = {
        DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH,
        DEV_FAN, DEV_WINDOW, DEV_DOOR, DEV_CURTAIN,
    };
    if (itemIndex < 0 || itemIndex >= UI_DEVICE_ITEMS) {
        return;
    }
    device_toggle(devs[itemIndex], SRC_LOCAL_KEY);
}

static void auto_ok_cb(void)
{
    const bool now = !automation_is_enabled();
    automation_set_enabled(now);
    automation_save();
    ESP_LOGI(TAG, "auto mode -> %s (by adkey)", now ? "ON" : "OFF");
}

/* ---------- 每帧动态数据刷新 ---------- */

static const char *const s_dev_cn[UI_DEVICE_ITEMS] = {
    "客厅灯", "厨房灯", "卧室灯", "浴室灯", "风扇", "窗户", "门", "窗帘",
};

static void refresh_home(void)
{
    char buf[40];
    const sensor_data_t *s = sensor_get_last();

    if (s->valid_temp) {
        snprintf(buf, sizeof(buf), "%.1fC %d%%", (double)s->temperature, (int)(s->humidity + 0.5f));
    } else {
        snprintf(buf, sizeof(buf), "--.-C --%%");
    }
    g_pageHome->tempHumiStr = buf;

    snprintf(buf, sizeof(buf), "光照 %d%% 雨滴 %d%%",
             (int)(s->light_pct + 0.5f), (int)(s->rain_pct + 0.5f));
    g_pageHome->lightRainStr = buf;

    snprintf(buf, sizeof(buf), "%s  %s",
             wifi_is_connected() ? "WiFi OK" : "WiFi 断开",
             mqtt_is_connected() ? "MQTT OK" : "MQTT 断开");
    g_pageHome->netStr = buf;

    snprintf(buf, sizeof(buf), "自动%s", automation_is_enabled() ? "开" : "关");
    g_pageHome->autoStr = buf;
}

static void refresh_selftest(void)
{
    const uint8_t cnt = selftest_get_count();
    const uint8_t cur = selftest_get_index();
    const uint8_t ph  = selftest_get_phase();

    g_pageSelfTest->statusStr = (ph == 1) ? "检测中" : ((ph == 2) ? "完成" : "等待");

    for (int i = 0; i < (int)cnt && i < UI_SELFTEST_ITEMS; i++) {
        std::string t = selftest_get_item_name((uint8_t)i);
        if (selftest_is_active() && i == (int)cur && ph == 1) {
            t += " 检测中";
        } else if (selftest_is_active() && i == (int)cur && ph == 2) {
            t += " 完成";
        }
        g_pageSelfTest->child[i]->title = t;
    }
}

static void refresh_device(void)
{
    static const device_id_t devs[UI_DEVICE_ITEMS] = {
        DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH,
        DEV_FAN, DEV_WINDOW, DEV_DOOR, DEV_CURTAIN,
    };
    char buf[40];
    for (int i = 0; i < UI_DEVICE_ITEMS; i++) {
        const bool on = device_get_power(devs[i]);
        snprintf(buf, sizeof(buf), "%s %s", s_dev_cn[i], on ? "[开]" : "[关]");
        g_pageDevice->child[i]->title = buf;
    }
}

static void refresh_sensor(void)
{
    char buf[32];
    const sensor_data_t *s = sensor_get_last();

    snprintf(buf, sizeof(buf), "温度 %.1fC", (double)s->temperature);
    g_pageSensor->tempStr = buf;
    snprintf(buf, sizeof(buf), "湿度 %d%%", (int)(s->humidity + 0.5f));
    g_pageSensor->humiStr = buf;
    snprintf(buf, sizeof(buf), "光照 %d%%", (int)(s->light_pct + 0.5f));
    g_pageSensor->lightStr = buf;
    snprintf(buf, sizeof(buf), "雨滴 %d%%", (int)(s->rain_pct + 0.5f));
    g_pageSensor->rainStr = buf;
}

static void refresh_auto(void)
{
    const automation_cfg_t *c = automation_get_cfg();
    char buf[40];

    snprintf(buf, sizeof(buf), "联动开关 %s", c->enabled ? "[开]" : "[关]");
    g_pageAuto->child[0]->title = buf;

    snprintf(buf, sizeof(buf), "光照开 %.1f 关 %.1f", (double)c->light_on_lux, (double)c->light_off_lux);
    g_pageAuto->child[1]->title = buf;
    snprintf(buf, sizeof(buf), "风扇开 %.1fC 关 %.1fC", (double)c->temp_fan_on_c, (double)c->temp_fan_off_c);
    g_pageAuto->child[2]->title = buf;
    snprintf(buf, sizeof(buf), "雨滴关窗 %.1f%%", (double)c->rain_pct);
    g_pageAuto->child[3]->title = buf;
    snprintf(buf, sizeof(buf), "风扇转速 %u%%", (unsigned)c->fan_auto_speed);
    g_pageAuto->child[4]->title = buf;
    snprintf(buf, sizeof(buf), "光照联动 %s", c->auto_light_enable ? "开" : "关");
    g_pageAuto->child[5]->title = buf;
    snprintf(buf, sizeof(buf), "温度联动 %s 雨滴 %s",
             c->auto_temp_enable ? "开" : "关", c->auto_rain_enable ? "开" : "关");
    g_pageAuto->child[6]->title = buf;
}

static void refresh_dynamic(void)
{
    refresh_home();
    refresh_selftest();
    refresh_device();
    refresh_sensor();
    refresh_auto();
}

/* ---------- UI 任务 ---------- */

static void astra_ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "astra UI task start");

    /* 注入桥接回调（必须在 astraCoreInit / 首次渲染之前） */
    astra_hal_set_flush_page_cb(flush_page_cb);
    astra_hal_set_key_down_cb(key_down_cb);
    astra_hal_set_beep_cb(beep_cb);

    astraCoreInit();
    astraSelfTestSetOkCb(selftest_ok_cb);
    astraSelfTestSetEnterCb(selftest_enter);
    astraSelfTestSetExitCb(selftest_exit);
    astraDeviceSetOkCb(device_ok_cb);
    astraAutoSetOkCb(auto_ok_cb);
    ESP_LOGI(TAG, "astra UI ready");

    for (;;) {
        HAL::keyScan();
        refresh_dynamic();
        astraLauncher->update();

        /* 语音提示弹窗（ESP-SR 识别任务写入，本任务消费）：
         * 先拷贝文字再清零 pending，避免弹窗期间文字被下一条覆盖。
         * 时长 1.2s（2026-10-02 用户反馈：2s 时第二条指令来了还在显示第一条）。 */
        if (g_voice_ui_note.pending) {
            std::string note(g_voice_ui_note.text);
            g_voice_ui_note.pending = 0;
            astraLauncher->popInfo(note, 1200);
        }

        vTaskDelay(pdMS_TO_TICKS(1));   /* 1000Hz 节拍下 1ms */
    }
}

extern "C" void astra_ui_start(void)
{
    /* 栈给大一点：u8g2 画布渲染 + 阻塞弹窗循环都在这个任务里 */
    if (xTaskCreate(astra_ui_task, "astra_ui", 12288, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create astra ui task failed");
    }
}
