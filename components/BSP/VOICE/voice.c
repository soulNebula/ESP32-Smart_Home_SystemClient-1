/*
 * 模块：
 *   语音总入口。管着一根串口接外面的语音模块，收到话就翻译成命令，
 *   交给 main.c 的 voice_on_cmd 去开灯开风扇那些事。
 *   自己向下调 board_config.h 里定好的引脚参数，
 *   换用麦克风离线识别时，这里改调 voice_esp_sr.c。
 *
 * 能做这些：
 *   初始化语音串口
 *   收字节翻成命令
 *   收到就交给上层
 *   自己造一条命令
 *   发文字让模块报
 *   报温度湿度
 *   看模块在不在线
 *
 * 几句说明：
 *   收语音这个任务的栈不能小，给了 6144。命令一到，回调会一路走到
 *   设备状态和上报（里面还有 JSON 拼装），那条链在 3072 字节的栈上
 *   实测栈溢出崩溃过，所以这里必须留够。
 *   模块有两种接法：外接串口语音模块（默认），或者一颗麦克风做离线
 *   识别，开关一开就换过去，二选一。换过去之后故意不再开串口，
 *   少一个会失败的地方，引脚也腾出来。两种接法认出来的命令都走
 *   同一个口子出去，上层完全感觉不到换了谁。
 *   模块没插上不会报错，只是收不到话，别的功能照常。没数据时读串口
 *   会超时返回，任务安静空转，不刷屏。
 *   在线判定看的是"真模块有没有发字节"。拔掉或者断电之后三十秒
 *   收不到字节，就自动算离线；插回来收到字节又变在线。
 *   自己注入的命令不算在线，因为那不是真模块说的。
 *   命令号的对照表顺序和条数必须和 voice.h 里的枚举一模一样，
 *   漏一个或者错位，编译就直接报错，不会拖到运行时才发现
 *   "说开灯结果开了风扇"。
 *   字节对照表是给只发一个字节的老模块用的：0x01 起一条接一条
 *   对着枚举排，模块换了只改这张表，解析逻辑一行都不用动。
 *   串口解析很宽容：先按单字节号认，再按一整行文字认，都不像就丢掉。
 *   不认识的行和越界的字节都静默丢弃，不影响正在跑的业务。
 *   播报靠模块自己念，我们只负责把文本发出去，末尾补一个换行。
 *   模块不认这段文本也没关系，它会自己忽略，不会卡住我们。
 *   拼播报文字用整数，不用小数格式：本工程可能开了省空间的格式化
 *   选项，那时候小数会打成空的。
 *   报温度湿度时传负数表示这一项不报，两项都负就当参数不对。
 *   语音起不来不算致命错，只记一条警告。要是返回错误，开机自检会把
 *   语音记成一笔失败，日志上很吓人，可系统其实好好的。
 *   模块必须和板子共地，不然收不到字节。
 */
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
#include "voice_internal.h"   /* 功能：内部下发口 */
#include "voice_esp_sr.h"     /* 功能：换识别来源用 */
#include "board_config.h"

#include "sdkconfig.h"

static const char *TAG = "voice";

/* 功能：收语音的死数 */
#define VOICE_TASK_STACK_SIZE   6144    /* 功能：收语音任务栈 */
#define VOICE_TASK_PRIORITY     3       /* 功能：收语音优先级 */
#define VOICE_READ_TIMEOUT_MS   100     /* 功能：没数据等多久 */
#define VOICE_RX_CHUNK          128     /* 功能：一次读多少字节 */
#define VOICE_TX_BUF_SIZE       256     /* 功能：发的先存这里 */
#define VOICE_LINE_MAX          64      /* 功能：一行最长多少字节 */
#define VOICE_ONLINE_TIMEOUT_US (30LL * 1000 * 1000)  /* 功能：多久算掉线 */

/* 功能：命令词的对照表 */
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

/* 功能：字节查命令 */
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

/* 功能：记在不在线 */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static voice_cmd_cb_t s_cb;         /* 功能：上层留的回话口 */
static void          *s_cb_user;
static bool           s_inited;     /* 功能：开过没，防重开 */

static bool    s_online;            /* 功能：收到过字节没 */
static int64_t s_last_rx_us;        /* 功能：上次收到的时刻 */

static char   s_line[VOICE_LINE_MAX];   /* 功能：攒一行文字 */
static size_t s_line_len;

/* 功能：所有语音都从这走 */
void voice_dispatch(voice_cmd_t cmd)
{
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return;     /* 功能：没认出来就丢掉 */
    }

    ESP_LOGI(TAG, "voice cmd: %s", voice_cmd_name(cmd));

    if (s_cb != NULL) {
        s_cb(cmd, s_cb_user);
    }
}

/* 功能：字节翻命令 */
static voice_cmd_t voice_seq_lookup(uint8_t byte)
{
    const int idx = (int)byte - 1;      /* 功能：0x01 排第一条 */

    if (idx < 0 || idx >= (int)(sizeof(voice_seq) / sizeof(voice_seq[0]))) {
        ESP_LOGD(TAG, "unknown voice seq byte 0x%02X, dropped", (unsigned)byte);
        return VOICE_CMD_NONE;
    }
    return voice_seq[idx];
}

/* 功能：逐字节认语音 */
static void voice_parse_byte(uint8_t byte)
{
    /* 功能：像单字节号 */
    if (byte != '\r' && byte != '\n' && (byte < 0x20 || byte > 0x7E)) {
        s_line_len = 0;                 /* 功能：半截文字不要了 */
        voice_dispatch(voice_seq_lookup(byte));
        return;
    }

    /* 功能：能读的字先攒行 */
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
        return;                         /* 功能：空行跳过 */
    }

    if (s_line_len < VOICE_LINE_MAX - 1) {
        s_line[s_line_len++] = (char)byte;
    } else {
        /* 功能：满了先当一条试 */
        s_line[VOICE_LINE_MAX - 1] = '\0';
        voice_dispatch(voice_cmd_from_name(s_line));
        s_line_len = 0;
    }
}

/* 功能：记下刚收到字节 */
static void voice_mark_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_online     = true;
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_mux);
}

/* 功能：守着串口收字节 */
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
            /* 功能：收到字节就算在线 */
            voice_mark_online();
            for (int i = 0; i < n; i++) {
                voice_parse_byte(buf[i]);
            }
        } else if (n < 0) {
            /* 功能：读错了歇一下 */
            vTaskDelay(pdMS_TO_TICKS(VOICE_READ_TIMEOUT_MS));
        }
        /* 功能：没人说话就静等 */
    }
}

/* 功能：开机准备好语音 */
esp_err_t voice_init(void)
{
    if (s_inited) {
        return ESP_OK;              /* 功能：第二次直接走 */
    }

#if CONFIG_APP_VOICE_SOURCE_ESP_SR
    /* 功能：改用麦克风识别 */
    ESP_LOGI(TAG, "语音识别来源 = INMP441(I²S) + ESP-SR 离线识别"
                  "（ASRPRO UART1 通道本次不初始化）");

    /* 功能：识别起不来也不崩 */
    const esp_err_t sr_err = voice_esp_sr_start();
    if (sr_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR 启动失败 (%s)：语音控制不可用，"
                      "但系统其它功能（含 say 注入、串口调试台）全部照常",
                 esp_err_to_name(sr_err));
        /* 功能：只报警不当错 */
    } else {
        ESP_LOGI(TAG, "ESP-SR 已就绪：唤醒词「%s」", voice_esp_sr_wake_word());
    }

    s_inited = true;
    return ESP_OK;
#else
    /* 功能：用外接语音模块 */
    const uart_config_t uart_cfg = {
        .baud_rate  = BSP_VOICE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,    /* 功能：时钟这么选省事 */
    };

    /* 功能：先配速度和脚 */
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

    /* 功能：留好收发缓冲 */
    err = uart_driver_install(BSP_VOICE_UART_PORT,
                              BSP_VOICE_RX_BUF_SIZE, VOICE_TX_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 功能：起任务守串口 */
    if (xTaskCreate(voice_rx_task, "voice_rx", VOICE_TASK_STACK_SIZE, NULL,
                    VOICE_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create voice rx task failed");
        uart_driver_delete(BSP_VOICE_UART_PORT);    /* 功能：起不来就退回 */
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok (语音识别来源 = 外部 ASRPRO 模块，UART%d)", BSP_VOICE_UART_PORT);
    return ESP_OK;
#endif
}
/* 功能：上层留下回话口 */
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

/* 功能：发文字让模块念 */
esp_err_t voice_speak(const char *text)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 功能：从串口发出去 */
    const int written = uart_write_bytes(BSP_VOICE_UART_PORT, text, strlen(text));
    if (written < 0) {
        ESP_LOGW(TAG, "uart_write_bytes failed, speak dropped");
        return ESP_FAIL;
    }
    uart_write_bytes(BSP_VOICE_UART_PORT, "\r\n", 2);

    ESP_LOGD(TAG, "speak: %s", text);
    return ESP_OK;
}

/* 功能：报温度和湿度 */
esp_err_t voice_speak_temp(float temp, float humi)
{
    char   text[64] = { 0 };    /* 功能：先把整句清空 */
    size_t n = 0;

    if (temp >= 0.0f) {
        const int t10 = (int)(temp * 10.0f + 0.5f);   /* 功能：留一位小数 */
        const int w   = snprintf(text, sizeof(text), "当前温度%d.%d度", t10 / 10, t10 % 10);
        n = (w > 0) ? (size_t)w : 0;
    }

    if (humi >= 0.0f && n < sizeof(text)) {
        const int h_int = (int)(humi + 0.5f);         /* 功能：湿度按整数报 */
        snprintf(text + n, sizeof(text) - n, "%s湿度%d%%",
                 (n > 0) ? "，" : "当前", h_int);
    }

    if (text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;   /* 功能：两个都没给 */
    }

    /* 功能：交给发字的口 */
    return voice_speak(text);
}

/* 功能：自己造一条命令 */
esp_err_t voice_inject_cmd(voice_cmd_t cmd)
{
    /* 功能：和真模块同一路 */
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 功能：注入不算在线 */
    voice_dispatch(cmd);
    return ESP_OK;
}

/* 功能：命令号翻名字 */
const char *voice_cmd_name(voice_cmd_t cmd)
{
    if ((int)cmd < 0 || cmd >= VOICE_CMD_MAX) {
        return name_table[VOICE_CMD_NONE];      /* 功能：越界都给 none */
    }
    return name_table[cmd];
}

/* 功能：名字翻回命令号 */
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

/* 功能：看模块还接着没 */
bool voice_is_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    const bool    online  = s_online;
    const int64_t last_us = s_last_rx_us;
    portEXIT_CRITICAL(&s_state_mux);

    if (!online) {
        return false;
    }

    /* 功能：久了就判掉线 */
    if ((esp_timer_get_time() - last_us) > VOICE_ONLINE_TIMEOUT_US) {
        portENTER_CRITICAL(&s_state_mux);
        s_online = false;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGW(TAG, "no data for 30s, voice module considered offline");
        return false;
    }

    return true;
}
