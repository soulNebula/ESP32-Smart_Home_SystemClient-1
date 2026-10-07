#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "voice.h"
// 内部下发口
#include "voice_internal.h"
// 换识别来源用
#include "voice_esp_sr.h"
#include "board_config.h"

#include "sdkconfig.h"

static const char *TAG = "voice";

// 收语音的死数
// 收语音任务栈
#define VOICE_TASK_STACK_SIZE   6144
// 收语音优先级
#define VOICE_TASK_PRIORITY     3
// 没数据等多久
#define VOICE_READ_TIMEOUT_MS   100
// 一次读多少字节
#define VOICE_RX_CHUNK          128
// 发的先存这里
#define VOICE_TX_BUF_SIZE       256
// 一行最长多少字节
#define VOICE_LINE_MAX          64
// 多久算掉线
#define VOICE_ONLINE_TIMEOUT_US (30LL * 1000 * 1000)

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

// 字节查命令
static const voice_cmd_t voice_seq[] = {
    VOICE_CMD_LED_LIVING_ON,
    VOICE_CMD_LED_LIVING_OFF,
    VOICE_CMD_LED_KITCHEN_ON,
    VOICE_CMD_LED_KITCHEN_OFF,
    VOICE_CMD_LED_BEDROOM_ON,
    VOICE_CMD_LED_BEDROOM_OFF,
    VOICE_CMD_LED_BATH_ON,
    VOICE_CMD_LED_BATH_OFF,
    VOICE_CMD_LED_ALL_ON,
    VOICE_CMD_LED_ALL_OFF,
    VOICE_CMD_FAN_ON,
    VOICE_CMD_FAN_OFF,
    VOICE_CMD_WINDOW_OPEN,
    VOICE_CMD_WINDOW_CLOSE,
    VOICE_CMD_DOOR_OPEN,
    VOICE_CMD_DOOR_CLOSE,
    VOICE_CMD_CURTAIN_OPEN,
    VOICE_CMD_CURTAIN_CLOSE,
    VOICE_CMD_QUERY_TEMP,
    VOICE_CMD_QUERY_HUMI,
    VOICE_CMD_QUERY_LIGHT,
    VOICE_CMD_QUERY_ALL,
    VOICE_CMD_QUERY_STATUS,
    VOICE_CMD_AUTO_ON,
    VOICE_CMD_AUTO_OFF,
};

_Static_assert(sizeof(voice_seq) / sizeof(voice_seq[0]) == VOICE_CMD_MAX - 1,
               "voice_seq[] 必须覆盖 VOICE_CMD_NONE 之后的全部指令（VOICE_CMD_MAX - 1 条）");

// 记在不在线
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

// 上层留的回话口
static voice_cmd_cb_t s_cb;
static void          *s_cb_user;
// 开过没，防重开
static bool           s_inited;

// 收到过字节没
static bool    s_online;
// 上次收到的时刻
static int64_t s_last_rx_us;

// 攒一行文字
static char   s_line[VOICE_LINE_MAX];
static size_t s_line_len;

// 所有语音都从这走
void voice_dispatch(voice_cmd_t cmd)
{
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        // 没认出来就丢掉
        return;
    }

    ESP_LOGI(TAG, "voice cmd: %s", voice_cmd_name(cmd));

    if (s_cb != NULL) {
        s_cb(cmd, s_cb_user);
    }
}

// 字节翻命令
static voice_cmd_t voice_seq_lookup(uint8_t byte)
{
    // 0x01 排第一条
    const int idx = (int)byte - 1;

    if (idx < 0 || idx >= (int)(sizeof(voice_seq) / sizeof(voice_seq[0]))) {
        ESP_LOGD(TAG, "unknown voice seq byte 0x%02X, dropped", (unsigned)byte);
        return VOICE_CMD_NONE;
    }
    return voice_seq[idx];
}

// 逐字节认语音
static void voice_parse_byte(uint8_t byte)
{
    // 像单字节号
    if (byte != '\r' && byte != '\n' && (byte < 0x20 || byte > 0x7E)) {
        // 半截文字不要了
        s_line_len = 0;
        voice_dispatch(voice_seq_lookup(byte));
        return;
    }

    // 能读的字先攒行
    if (byte == '\r' || byte == '\n') {
        if (s_line_len > 0) {
            s_line[s_line_len] = '\0';
            const voice_cmd_t cmd = voice_cmd_from_name(s_line);
            if (cmd == VOICE_CMD_NONE) {
                ESP_LOGD(TAG, "unknown voice text line: \"%s\", dropped", s_line);
            }
            voice_dispatch(cmd);
            s_line_len = 0;
        }
        // 空行跳过
        return;
    }

    if (s_line_len < VOICE_LINE_MAX - 1) {
        s_line[s_line_len++] = (char)byte;
    } else {
        // 满了先当一条试
        s_line[VOICE_LINE_MAX - 1] = '\0';
        voice_dispatch(voice_cmd_from_name(s_line));
        s_line_len = 0;
    }
}

// 记下刚收到字节
static void voice_mark_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_online     = true;
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_mux);
}

// 守着串口收字节
static void voice_rx_task(void *arg)
{
    (void)arg;

    uint8_t buf[VOICE_RX_CHUNK];

    ESP_LOGI(TAG, "rx task start: UART%d baud=%d tx=GPIO%d rx=GPIO%d",
             BSP_VOICE_UART_PORT, BSP_VOICE_BAUD,
             (int)BSP_VOICE_TX_GPIO, (int)BSP_VOICE_RX_GPIO);
    ESP_LOGI(TAG, "等待语音模块数据（模块没接也不会报错，可用 voice_inject_cmd() 先跑业务）");

    while (1) {
        const int n = uart_read_bytes(BSP_VOICE_UART_PORT, buf, sizeof(buf),
                                      pdMS_TO_TICKS(VOICE_READ_TIMEOUT_MS));

        if (n > 0) {
            // 收到字节就算在线
            voice_mark_online();
            for (int i = 0; i < n; i++) {
                voice_parse_byte(buf[i]);
            }
        } else if (n < 0) {
            // 读错了歇一下
            vTaskDelay(pdMS_TO_TICKS(VOICE_READ_TIMEOUT_MS));
        }
        // 没人说话就静等
    }
}

// 开机准备好语音
esp_err_t voice_init(void)
{
    if (s_inited) {
        // 第二次直接走
        return ESP_OK;
    }

#if CONFIG_APP_VOICE_SOURCE_ESP_SR
    // 改用麦克风识别
    ESP_LOGI(TAG, "语音识别来源 = INMP441(I²S) + ESP-SR 离线识别"
                  "（ASRPRO UART1 通道本次不初始化）");

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
#else
    // 用外接语音模块
    const uart_config_t uart_cfg = {
        .baud_rate  = BSP_VOICE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        // 时钟这么选省事
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 先配速度和脚
    esp_err_t err = uart_param_config(BSP_VOICE_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(BSP_VOICE_UART_PORT,
                       BSP_VOICE_TX_GPIO, BSP_VOICE_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    // 留好收发缓冲
    err = uart_driver_install(BSP_VOICE_UART_PORT,
                              BSP_VOICE_RX_BUF_SIZE, VOICE_TX_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // 起任务守串口
    if (xTaskCreate(voice_rx_task, "voice_rx", VOICE_TASK_STACK_SIZE, NULL,
                    VOICE_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create voice rx task failed");
        // 起不来就退回
        uart_driver_delete(BSP_VOICE_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok (语音识别来源 = 外部 ASRPRO 模块，UART%d)", BSP_VOICE_UART_PORT);
    return ESP_OK;
#endif
}
// 上层留下回话口
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

// 发文字让模块念
esp_err_t voice_speak(const char *text)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // 从串口发出去
    const int written = uart_write_bytes(BSP_VOICE_UART_PORT, text, strlen(text));
    if (written < 0) {
        ESP_LOGW(TAG, "uart_write_bytes failed, speak dropped");
        return ESP_FAIL;
    }
    uart_write_bytes(BSP_VOICE_UART_PORT, "\r\n", 2);

    ESP_LOGD(TAG, "speak: %s", text);
    return ESP_OK;
}

// 报温度和湿度
esp_err_t voice_speak_temp(float temp, float humi)
{
    // 先把整句清空
    char   text[64] = { 0 };
    size_t n = 0;

    if (temp >= 0.0f) {
        // 留一位小数
        const int t10 = (int)(temp * 10.0f + 0.5f);
        const int w   = snprintf(text, sizeof(text), "当前温度%d.%d度", t10 / 10, t10 % 10);
        n = (w > 0) ? (size_t)w : 0;
    }

    if (humi >= 0.0f && n < sizeof(text)) {
        // 湿度按整数报
        const int h_int = (int)(humi + 0.5f);
        snprintf(text + n, sizeof(text) - n, "%s湿度%d%%",
                 (n > 0) ? "，" : "当前", h_int);
    }

    if (text[0] == '\0') {
        // 两个都没给
        return ESP_ERR_INVALID_ARG;
    }

    // 交给发字的口
    return voice_speak(text);
}

// 自己造一条命令
esp_err_t voice_inject_cmd(voice_cmd_t cmd)
{
    // 和真模块同一路
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // 注入不算在线
    voice_dispatch(cmd);
    return ESP_OK;
}

// 命令号翻名字
const char *voice_cmd_name(voice_cmd_t cmd)
{
    if ((int)cmd < 0 || cmd >= VOICE_CMD_MAX) {
        // 越界都给 none
        return name_table[VOICE_CMD_NONE];
    }
    return name_table[cmd];
}

// 名字翻回命令号
voice_cmd_t voice_cmd_from_name(const char *name)
{
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

// 看模块还接着没
bool voice_is_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    const bool    online  = s_online;
    const int64_t last_us = s_last_rx_us;
    portEXIT_CRITICAL(&s_state_mux);

    if (!online) {
        return false;
    }

    // 久了就判掉线
    if ((esp_timer_get_time() - last_us) > VOICE_ONLINE_TIMEOUT_US) {
        portENTER_CRITICAL(&s_state_mux);
        s_online = false;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGW(TAG, "no data for 30s, voice module considered offline");
        return false;
    }

    return true;
}
