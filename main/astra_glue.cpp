// UI框架与智能家居业务之间通信层
// C++

#include <cstdio>
#include <cstring>
#include <string>

// FreeRTOS
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
#include "voice_esp_sr.h"   
}

// UI框架
#include "astra_rocket.h"
#include "astra_glue.h"

// UI 页面对象
static const char *TAG = "astra_glue";

// 键位映射：astra 键序 {UP, DOWN, LEFT, RIGHT, OK} → 本机五键键盘
static const adkey_id_t KEY_MAP[key::KEY_NUM] = { ADKEY_3, ADKEY_4, ADKEY_1, ADKEY_2, ADKEY_OK };

// OLED 刷新回调
static void flush_page_cb(uint8_t page, const uint8_t *data) {
    oled_write_page(page, data);
}

// 按键按下回调
static bool key_down_cb(uint8_t idx) {
    if (idx >= key::KEY_NUM) {
        return false;
    }
    return adkey_is_pressed(KEY_MAP[idx]);
}

// 蜂鸣器回调
static void beep_cb(float freq) {
    (void)freq;   
}

// 自检页面回调
static void selftest_ok_cb(int itemIndex) {
    if (itemIndex >= 0 && itemIndex < (int)selftest_get_count()) {
        selftest_run_index((uint8_t)itemIndex);
    }
}

// 设备页面回调
static void device_ok_cb(int itemIndex) {
    static const device_id_t devs[UI_DEVICE_ITEMS] = {
        DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH,
        DEV_FAN, DEV_WINDOW, DEV_DOOR, DEV_CURTAIN,
    };
    if (itemIndex < 0 || itemIndex >= UI_DEVICE_ITEMS) {
        return;
    }
    device_toggle(devs[itemIndex], SRC_LOCAL_KEY);
}

static void auto_ok_cb(void) {
    const bool now = !automation_is_enabled();
    automation_set_enabled(now);
    automation_save();
    ESP_LOGI(TAG, "auto mode -> %s (by adkey)", now ? "ON" : "OFF");
}

static const char *const s_dev_cn[UI_DEVICE_ITEMS] = {
    "客厅灯", "厨房灯", "卧室灯", "浴室灯", "风扇", "窗户", "门", "窗帘(停用)",
};

// 刷新各页面的动态数据
static void refresh_home(void) {
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

// 刷新自检页面的动态数据
static void refresh_selftest(void) {
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

// 刷新设备页面的动态数据
static void refresh_device(void) {
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

// 刷新传感器页面的动态数据
static void refresh_sensor(void) {
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

// 刷新自动控制页面的动态数据
static void refresh_auto(void) {
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

// 刷新所有页面的动态数据
static void refresh_dynamic(void) {
    refresh_home();
    refresh_selftest();
    refresh_device();
    refresh_sensor();
    refresh_auto();
}

// UI
static void astra_ui_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "astra UI task start");
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

        // 语音提示弹窗
        if (g_voice_ui_note.pending) {
            std::string note(g_voice_ui_note.text);
            g_voice_ui_note.pending = 0;
            astraLauncher->popInfo(note, 1200);
        }
        // 1000Hz 节拍下 1ms
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 启动任务
extern "C" void astra_ui_start(void) {
    // 栈：u8g2 画布渲染 + 阻塞弹窗循环
    if (xTaskCreate(astra_ui_task, "astra_ui", 12288, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create astra ui task failed");
    }
}
