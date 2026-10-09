// 语音驱动：命令总线 + 名字表 + 回调
// 识别来源固定为 INMP441(I²S) + ESP-SR 离线识别
// （原 ASRPRO 串口方案已移除：那半套 UART 接收与语音播报在本方案下从未生效）

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "voice.h"
// 内部下发口
#include "voice_internal.h"
#include "voice_esp_sr.h"

static const char *TAG = "voice";

// 命令词的对照表
static const char *name_table[VOICE_CMD_MAX] = {
    "none",
    "led_living_on",
    "led_living_off",
    "led_kitchen_on",
    "led_kitchen_off",
    "led_bedroom_on",
    "led_bedroom_off",
    "led_bath_on",
    "led_bath_off",
    "led_all_on",
    "led_all_off",
    "fan_on",
    "fan_off",
    "window_open",
    "window_close",
    "door_open",
    "door_close",
    "curtain_open",
    "curtain_close",
    "query_temp",
    "query_humi",
    "query_light",
    "query_all",
    "query_status",
    "auto_on",
    "auto_off",
};

_Static_assert(sizeof(name_table) / sizeof(name_table[0]) == VOICE_CMD_MAX,
               "name_table[] 条目数必须等于 VOICE_CMD_MAX（与 voice.h 枚举顺序一一对应）");

// 上层留的回话口
static voice_cmd_cb_t s_cb;
static void          *s_cb_user;
// 开过没，防重开
static bool           s_inited;

// 所有语音都从这走
void voice_dispatch(voice_cmd_t cmd) {
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        // 没认出来就丢掉
        return;
    }

    ESP_LOGI(TAG, "voice cmd: %s", voice_cmd_name(cmd));

    if (s_cb != NULL) {
        s_cb(cmd, s_cb_user);
    }
}

// 开机准备好语音
esp_err_t voice_init(void) {
    if (s_inited) {
        // 第二次直接走
        return ESP_OK;
    }

    ESP_LOGI(TAG, "语音识别来源 = INMP441(I²S) + ESP-SR 离线识别");

    // 识别起不来也不崩
    const esp_err_t sr_err = voice_esp_sr_start();
    if (sr_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR 启动失败 (%s)：语音控制不可用，"
                      "但系统其它功能（含 say 注入、串口调试台）全部照常",
                 esp_err_to_name(sr_err));
        // 只报警不当错
    } else {
        ESP_LOGI(TAG, "ESP-SR 已就绪：唤醒词「%s」", voice_esp_sr_wake_word());
    }

    s_inited = true;
    return ESP_OK;
}

// 上层留下回话口
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data) {
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

// 自己造一条命令
esp_err_t voice_inject_cmd(voice_cmd_t cmd) {
    // 和真模块同一路
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // 注入不算在线
    voice_dispatch(cmd);
    return ESP_OK;
}

// 命令号翻名字
const char *voice_cmd_name(voice_cmd_t cmd) {
    if ((int)cmd < 0 || cmd >= VOICE_CMD_MAX) {
        // 越界都给 none
        return name_table[VOICE_CMD_NONE];
    }
    return name_table[cmd];
}

// 名字翻回命令号
voice_cmd_t voice_cmd_from_name(const char *name) {
    if (name == NULL) {
        return VOICE_CMD_NONE;
    }

    for (int i = 0; i < VOICE_CMD_MAX; i++) {
        if (strcmp(name, name_table[i]) == 0) {
            return (voice_cmd_t)i;
        }
    }
    return VOICE_CMD_NONE;
}
