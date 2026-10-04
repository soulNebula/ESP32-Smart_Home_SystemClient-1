/**
 * @file  voice.c
 * @brief 语音交互【抽象层】实现 —— 硬件没到也能把整条业务链跑通
 *
 * ===========================================================================
 *  设计意图（务必先看懂这一层，再换真实模块）
 * ===========================================================================
 *  本模块把「说一句话」抽象成 voice_cmd_t 枚举 + 一个回调 voice_cmd_cb_t。
 *  两个来源必须走【完全相同】的下游路径：
 *
 *      A. 真实模块  UART 收到字节 → 解析/翻译成 voice_cmd_t ─┐
 *                                                             ├─→ voice_dispatch()
 *      B. 手动注入  voice_inject_cmd(cmd) ────────────────────┘        │
 *                                                                      ↓
 *                                                      日志 + 注册的回调（App 层）
 *
 *  · 手动注入（按键 / 串口调试 / MQTT 都能调）让整条业务链在模块到货前就能验证。
 *  · 将来换成"离线 ESP-SR（唤醒词 + 命令词）"或"在线云识别"，
 *    只需要替换【本文件内部】的采集/解析实现，把它翻译成 voice_cmd_t 后调
 *    同一个 voice_dispatch() 即可 —— App 层一行都不用改。
 *
 * ===========================================================================
 *  串口协议（容错解析，因为不同厂家模块的协议不一样）
 * ===========================================================================
 *  1) 纯文本行：以 '\n' 或 '\r' 结尾的 ASCII 字符串，内容就是指令名。
 *        例如  模块发 "led_living_on\r\n"  →  VOICE_CMD_LED_LIVING_ON
 *        用 voice_cmd_from_name() 反查（大小写敏感，必须和 voice_cmd_name()
 *        返回的字符串完全一致）。
 *  2) 单字节序号：某些模块（LD3320 一类）只发一个字节表示"第几个命令词"。
 *        0x01 → VOICE_CMD_LED_LIVING_ON，0x02 → VOICE_CMD_LED_LIVING_OFF ……
 *        映射表就是下面的 voice_seq[]，
 *        ★【模块换了、或者你在上位机里改了命令词顺序，只改这张表就行】★
 *        越界字节（0x00 或 > 表长）当无效丢弃，不影响正在跑的业务。
 *
 *  注：本模块不做"帧头 + 校验和"的强协议校验 —— 目标是现在就能跑通，
 *      等真模块到货后按它的手册把 voice_parse_byte() 换成真正的协议解析即可。
 *
 * ===========================================================================
 *  串口参数
 * ===========================================================================
 *  UART1，BSP_VOICE_TX_GPIO(ESP32 TX → 模块 RXD)、BSP_VOICE_RX_GPIO(ESP32 RX
 *  ← 模块 TXD)，波特率 BSP_VOICE_BAUD，8N1，无硬件流控（模块一般只用 TX/RX/GND）。
 *  ⚠ 模块必须和开发板【共地】，否则收不到字节。
 *  ⚠ 模块没接线时：uart_read_bytes() 只会 100ms 超时返回 0，接收任务静默空转，
 *    不报错、不刷屏日志；voice_is_online() 也会自动变回离线。
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
#include "voice_internal.h"   /* 把 voice_dispatch() 开给同目录的 voice_esp_sr.c */
#include "voice_esp_sr.h"     /* voice_esp_sr_start() / voice_esp_sr_wake_word() */
#include "board_config.h"

#include "sdkconfig.h"

static const char *TAG = "voice";

/* -------------------------------------------------------------------------- */
/*  参数                                                                       */
/* -------------------------------------------------------------------------- */
#define VOICE_TASK_STACK_SIZE   6144    /* 接收任务栈（字节）。
                                         * ★ 不能小于 4096：语音回调链会走到
                                         *   device_model → mqtt_publish_state
                                         *   （cJSON 递归 + 事件发布），3072 实测
                                         *   栈溢出 panic（语音命令后必崩）。 */
#define VOICE_TASK_PRIORITY     3       /* 接收任务优先级（低于按键扫描任务 4） */
#define VOICE_READ_TIMEOUT_MS   100     /* uart_read_bytes 超时：没数据就空转 */
#define VOICE_RX_CHUNK          128     /* 单次最多读多少字节 */
#define VOICE_TX_BUF_SIZE       256     /* TX 环形缓冲，避免 voice_speak 阻塞调用者 */
#define VOICE_LINE_MAX          64      /* 文本行缓冲（含结尾 '\0'） */
#define VOICE_ONLINE_TIMEOUT_US (30LL * 1000 * 1000)  /* 30s 没字节 → 判定离线 */

/* -------------------------------------------------------------------------- */
/*  指令名表                                                                   */
/*  ★★★ 必须与 voice.h 里 voice_cmd_t 的【顺序和条目数完全一致】★★★
 *  下面已逐条对照 voice.h（顺序: NONE, LIVING_ON/OFF, KITCHEN_ON/OFF,
 *  BEDROOM_ON/OFF, BATH_ON/OFF, ALL_ON/OFF, FAN_ON/OFF, WINDOW_OPEN/CLOSE,
 *  DOOR_OPEN/CLOSE, CURTAIN_OPEN/CLOSE, QUERY_TEMP/HUMI/LIGHT/ALL/STATUS,
 *  AUTO_ON/OFF，共 26 项 = VOICE_CMD_MAX）。
 *  用 [VOICE_CMD_MAX] 定长 + 编译期断言保护：漏写/错位会直接编译不过，
 *  不会拖到运行时才发现"说开灯结果开了风扇"。
 * -------------------------------------------------------------------------- */
static const char *name_table[VOICE_CMD_MAX] = {
    /* 0  */ "none",
    /* 1  */ "led_living_on",
    /* 2  */ "led_living_off",
    /* 3  */ "led_kitchen_on",
    /* 4  */ "led_kitchen_off",
    /* 5  */ "led_bedroom_on",
    /* 6  */ "led_bedroom_off",
    /* 7  */ "led_bath_on",
    /* 8  */ "led_bath_off",
    /* 9  */ "led_all_on",
    /* 10 */ "led_all_off",
    /* 11 */ "fan_on",
    /* 12 */ "fan_off",
    /* 13 */ "window_open",
    /* 14 */ "window_close",
    /* 15 */ "door_open",
    /* 16 */ "door_close",
    /* 17 */ "curtain_open",
    /* 18 */ "curtain_close",
    /* 19 */ "query_temp",
    /* 20 */ "query_humi",
    /* 21 */ "query_light",
    /* 22 */ "query_all",
    /* 23 */ "query_status",
    /* 24 */ "auto_on",
    /* 25 */ "auto_off",
};

_Static_assert(sizeof(name_table) / sizeof(name_table[0]) == VOICE_CMD_MAX,
               "name_table[] 条目数必须等于 VOICE_CMD_MAX（与 voice.h 枚举顺序一一对应）");

/* -------------------------------------------------------------------------- */
/*  单字节序号 → 指令 映射表                                                    */
/*                                                                             */
/*  某些模块（LD3320 等）收到识别结果后只吐一个字节表示"第几个命令词"。          */
/*  约定：字节 0x01 起，依次对应 VOICE_CMD_NONE 之后的第一个枚举值。            */
/*                                                                             */
/*    0x01 → VOICE_CMD_LED_LIVING_ON    0x0E → VOICE_CMD_WINDOW_CLOSE          */
/*    0x02 → VOICE_CMD_LED_LIVING_OFF   0x0F → VOICE_CMD_DOOR_OPEN             */
/*    0x03 → VOICE_CMD_LED_KITCHEN_ON   0x10 → VOICE_CMD_DOOR_CLOSE            */
/*    0x04 → VOICE_CMD_LED_KITCHEN_OFF  0x11 → VOICE_CMD_CURTAIN_OPEN          */
/*    0x05 → VOICE_CMD_LED_BEDROOM_ON   0x12 → VOICE_CMD_CURTAIN_CLOSE         */
/*    0x06 → VOICE_CMD_LED_BEDROOM_OFF  0x13 → VOICE_CMD_QUERY_TEMP            */
/*    0x07 → VOICE_CMD_LED_BATH_ON      0x14 → VOICE_CMD_QUERY_HUMI            */
/*    0x08 → VOICE_CMD_LED_BATH_OFF     0x15 → VOICE_CMD_QUERY_LIGHT           */
/*    0x09 → VOICE_CMD_LED_ALL_ON       0x16 → VOICE_CMD_QUERY_ALL             */
/*    0x0A → VOICE_CMD_LED_ALL_OFF      0x17 → VOICE_CMD_QUERY_STATUS          */
/*    0x0B → VOICE_CMD_FAN_ON           0x18 → VOICE_CMD_AUTO_ON               */
/*    0x0C → VOICE_CMD_FAN_OFF          0x19 → VOICE_CMD_AUTO_OFF              */
/*    0x0D → VOICE_CMD_WINDOW_OPEN                                              */
/*                                                                             */
/*  ★ 模块换了、或在上位机里改了命令词顺序，只改这张表                               
 *    （如果连枚举也变了，还要同步改上面的 name_table），解析逻辑一行都不用动。      */
/* -------------------------------------------------------------------------- */
static const voice_cmd_t voice_seq[] = {
    VOICE_CMD_LED_LIVING_ON,    /* 0x01 */
    VOICE_CMD_LED_LIVING_OFF,   /* 0x02 */
    VOICE_CMD_LED_KITCHEN_ON,   /* 0x03 */
    VOICE_CMD_LED_KITCHEN_OFF,  /* 0x04 */
    VOICE_CMD_LED_BEDROOM_ON,   /* 0x05 */
    VOICE_CMD_LED_BEDROOM_OFF,  /* 0x06 */
    VOICE_CMD_LED_BATH_ON,      /* 0x07 */
    VOICE_CMD_LED_BATH_OFF,     /* 0x08 */
    VOICE_CMD_LED_ALL_ON,       /* 0x09 */
    VOICE_CMD_LED_ALL_OFF,      /* 0x0A */
    VOICE_CMD_FAN_ON,           /* 0x0B */
    VOICE_CMD_FAN_OFF,          /* 0x0C */
    VOICE_CMD_WINDOW_OPEN,      /* 0x0D */
    VOICE_CMD_WINDOW_CLOSE,     /* 0x0E */
    VOICE_CMD_DOOR_OPEN,        /* 0x0F */
    VOICE_CMD_DOOR_CLOSE,       /* 0x10 */
    VOICE_CMD_CURTAIN_OPEN,     /* 0x11 */
    VOICE_CMD_CURTAIN_CLOSE,    /* 0x12 */
    VOICE_CMD_QUERY_TEMP,       /* 0x13 */
    VOICE_CMD_QUERY_HUMI,       /* 0x14 */
    VOICE_CMD_QUERY_LIGHT,      /* 0x15 */
    VOICE_CMD_QUERY_ALL,        /* 0x16 */
    VOICE_CMD_QUERY_STATUS,     /* 0x17 */
    VOICE_CMD_AUTO_ON,          /* 0x18 */
    VOICE_CMD_AUTO_OFF,         /* 0x19 */
};

_Static_assert(sizeof(voice_seq) / sizeof(voice_seq[0]) == VOICE_CMD_MAX - 1,
               "voice_seq[] 必须覆盖 VOICE_CMD_NONE 之后的全部指令（VOICE_CMD_MAX - 1 条）");

/* -------------------------------------------------------------------------- */
/*  内部状态                                                                   */
/* -------------------------------------------------------------------------- */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static voice_cmd_cb_t s_cb;         /* 只保留最后一个注册者 */
static void          *s_cb_user;
static bool           s_inited;     /* UART 是否已初始化（幂等标志） */

static bool    s_online;            /* 是否收到过字节（受 s_state_mux 保护） */
static int64_t s_last_rx_us;        /* 最近一次收到字节的时刻（受 s_state_mux 保护） */

static char   s_line[VOICE_LINE_MAX];   /* 文本行累积缓冲（只被接收任务访问） */
static size_t s_line_len;

/* -------------------------------------------------------------------------- */
/*  指令分发 —— A/B 两个来源的唯一汇合点                                        */
/* -------------------------------------------------------------------------- */
/**
 * @brief 执行一条语音指令：打日志 + 调注册的回调
 * @note  ⚠ 回调运行在【语音接收任务】上下文（voice_inject_cmd 则在【调用者任务】
 *        上下文）。回调里不要做阻塞操作，耗时动作请转投业务队列。
 * @note  非 static：ESP-SR 方案（voice_esp_sr.c）也调它，见 voice_internal.h
 */
void voice_dispatch(voice_cmd_t cmd)
{
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return;     /* VOICE_CMD_NONE：没识别出来 / 无效，静默返回 */
    }

    ESP_LOGI(TAG, "voice cmd: %s", voice_cmd_name(cmd));

    if (s_cb != NULL) {
        s_cb(cmd, s_cb_user);
    }
}

static voice_cmd_t voice_seq_lookup(uint8_t byte)
{
    const int idx = (int)byte - 1;      /* 0x01 → voice_seq[0] */

    if (idx < 0 || idx >= (int)(sizeof(voice_seq) / sizeof(voice_seq[0]))) {
        ESP_LOGD(TAG, "unknown voice seq byte 0x%02X, dropped", (unsigned)byte);
        return VOICE_CMD_NONE;
    }
    return voice_seq[idx];
}

/**
 * @brief 容错解析：把收到的每一个字节喂进来
 *        先试"单字节序号"，再试"文本行"，两种都试不出来就当无效数据丢弃。
 *        缓冲区永远不会越界（写入前必判 s_line_len < VOICE_LINE_MAX - 1）。
 */
static void voice_parse_byte(uint8_t byte)
{
    /* ---- 形式 2：单字节序号（不可打印 ASCII，且不是行结束符） ---- */
    if (byte != '\r' && byte != '\n' && (byte < 0x20 || byte > 0x7E)) {
        s_line_len = 0;                 /* 二进制协议来了：丢掉半截文本行 */
        voice_dispatch(voice_seq_lookup(byte));
        return;
    }

    /* ---- 形式 1：可打印 ASCII，累积成一行，遇到 '\r'/'\n' 才算一条 ---- */
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
        return;                         /* 空行（连续 \r\n）直接忽略 */
    }

    if (s_line_len < VOICE_LINE_MAX - 1) {
        s_line[s_line_len++] = (char)byte;
    } else {
        /* 缓冲区满了还没见到换行：把已有内容当一条指令试一次，然后清空 —— 防越界 */
        s_line[VOICE_LINE_MAX - 1] = '\0';
        voice_dispatch(voice_cmd_from_name(s_line));
        s_line_len = 0;
    }
}

/* -------------------------------------------------------------------------- */
/*  在线状态                                                                   */
/* -------------------------------------------------------------------------- */
static void voice_mark_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_online     = true;
    s_last_rx_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_mux);
}

/* -------------------------------------------------------------------------- */
/*  接收任务                                                                   */
/* -------------------------------------------------------------------------- */
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
            /* 收到任何字节就算"模块在线"（哪怕协议不认识），避免误判离线 */
            voice_mark_online();
            for (int i = 0; i < n; i++) {
                voice_parse_byte(buf[i]);
            }
        } else if (n < 0) {
            /* 驱动层报错（正常接线时基本不会发生）：歇一拍，不刷屏 */
            vTaskDelay(pdMS_TO_TICKS(VOICE_READ_TIMEOUT_MS));
        }
        /* n == 0：100ms 超时 = 模块没接线 / 没说话 → 静默空转，这是正常情况 */
    }
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */
esp_err_t voice_init(void)
{
    if (s_inited) {
        return ESP_OK;              /* 幂等：重复调用直接返回，不会重装驱动 */
    }

#if CONFIG_APP_VOICE_SOURCE_ESP_SR
    /* ======================================================================
     *  语音识别来源 = INMP441(I²S) + ESP-SR 离线识别
     * ======================================================================
     *  ★ 这里【故意不初始化 ASRPRO 的 UART1】：
     *    · 打开开关就说明用户换成了 INMP441 方案，ASRPRO 模块不接（或已拆掉）；
     *    · 不装 UART 驱动就不会有接收任务空转，也少一处可能失败的初始化；
     *    · GPIO21/GPIO47 随之释放，将来可作它用。
     *  识别结果由 voice_esp_sr.c 翻译成 voice_cmd_t 后调【同一个】
     *  voice_dispatch()，所以 App 层完全无感 —— 这就是"换识别引擎只改
     *  voice.c 内部实现"的落地方式。
     * ====================================================================== */
    ESP_LOGI(TAG, "语音识别来源 = INMP441(I²S) + ESP-SR 离线识别"
                  "（ASRPRO UART1 通道本次不初始化）");

    /* voice_esp_sr_start() 内部每一步失败都只告警：麦克风没接、模型没烧、
     * PSRAM 不足……都不会让系统崩，WiFi/MQTT/BLE/按键全部照常。 */
    const esp_err_t sr_err = voice_esp_sr_start();
    if (sr_err != ESP_OK) {
        ESP_LOGW(TAG, "ESP-SR 启动失败 (%s)：语音控制不可用，"
                      "但系统其它功能（含 say 注入、串口调试台）全部照常",
                 esp_err_to_name(sr_err));
        /* ★ 注意：这里仍然标记 s_inited = true 并返回 ESP_OK。
         *   理由：语音模块不可用【不是致命错误】（本工程一贯风格）。
         *   返回错误会让 board.c 把 voice 记成 [FAIL]，日志上很吓人，
         *   但其实系统完全能用 —— 只打 WARN 更准确。 */
    } else {
        ESP_LOGI(TAG, "ESP-SR 已就绪：唤醒词「%s」", voice_esp_sr_wake_word());
    }

    s_inited = true;
    return ESP_OK;
#else
    /* ======================================================================
     *  语音识别来源 = 外部 ASRPRO 模块（UART1）—— 这是工程的默认方案
     * ====================================================================== */
    const uart_config_t uart_cfg = {
        .baud_rate  = BSP_VOICE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,    /* IDF 5.x 推荐写法，替代旧的 UART_SCLK_APB */
    };

    /* 1. 参数 + 引脚 */
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

    /* 2. 安装驱动：RX 环形缓冲 BSP_VOICE_RX_BUF_SIZE，不用事件队列 */
    err = uart_driver_install(BSP_VOICE_UART_PORT,
                              BSP_VOICE_RX_BUF_SIZE, VOICE_TX_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. 起接收任务 */
    if (xTaskCreate(voice_rx_task, "voice_rx", VOICE_TASK_STACK_SIZE, NULL,
                    VOICE_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create voice rx task failed");
        uart_driver_delete(BSP_VOICE_UART_PORT);    /* 回滚，保证状态干净 */
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok (语音识别来源 = 外部 ASRPRO 模块，UART%d)", BSP_VOICE_UART_PORT);
    return ESP_OK;
#endif /* CONFIG_APP_VOICE_SOURCE_ESP_SR */
}
esp_err_t voice_register_cb(voice_cmd_cb_t cb, void *user_data)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cb      = cb;
    s_cb_user = user_data;
    return ESP_OK;
}

esp_err_t voice_speak(const char *text)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* 本模块只负责"把文本从 UART 发出去"，播报由模块自己完成。
     * · 模块支持 TTS（SU-03T / ASRPRO 部分固件）：会念出这句话。
     * · 模块只支持固定命令词：这一串不被识别，会被它【直接忽略】，无副作用
     *   （不会死机、不会卡任务、不会污染命令词识别）—— 放心调用。
     * 末尾补 "\r\n"：多数模块以换行作为一帧文本的结束标志。 */
    const int written = uart_write_bytes(BSP_VOICE_UART_PORT, text, strlen(text));
    if (written < 0) {
        ESP_LOGW(TAG, "uart_write_bytes failed, speak dropped");
        return ESP_FAIL;
    }
    uart_write_bytes(BSP_VOICE_UART_PORT, "\r\n", 2);

    ESP_LOGD(TAG, "speak: %s", text);
    return ESP_OK;
}

esp_err_t voice_speak_temp(float temp, float humi)
{
    /* 约定（和 main.c 里的调用方式对齐）：传【负数】= "这个值不播"。
     *     voice_speak_temp(26.5f, -1.0f)   → 只播温度："当前温度26.5度"
     *     voice_speak_temp(-100.0f, 58.0f) → 只播湿度："当前湿度58%"
     *     voice_speak_temp(26.5f, 58.0f)   → 都播："当前温度26.5度，湿度58%"
     * 室内温度/湿度永远不为负，用哨兵值是安全的；若以后要播室外零下温度，
     * 建议把接口改成带 bool 有效标志的版本（接口已冻结，暂时只能这样）。
     *
     * 拼字符串用【整数拆分】而不是 %f：若工程打开了 CONFIG_NEWLIB_NANO_FORMAT，
     * printf 家族会把 %f 打成空串/乱码，整数方案在任何配置下都稳定。 */
    char   text[64] = { 0 };    /* 零初始化：下面的 text[0] 判空才有意义 */
    size_t n = 0;

    if (temp >= 0.0f) {
        const int t10 = (int)(temp * 10.0f + 0.5f);   /* 保留 1 位小数（四舍五入） */
        const int w   = snprintf(text, sizeof(text), "当前温度%d.%d度", t10 / 10, t10 % 10);
        n = (w > 0) ? (size_t)w : 0;
    }

    if (humi >= 0.0f && n < sizeof(text)) {
        const int h_int = (int)(humi + 0.5f);         /* 湿度按整数播报 */
        snprintf(text + n, sizeof(text) - n, "%s湿度%d%%",
                 (n > 0) ? "，" : "当前", h_int);
    }

    if (text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;   /* 温度湿度都没给有效值 */
    }

    /* ⚠ 若你的模块【只支持固定命令词】、不能播任意文本，把这段改成"播报指令序号"：
     *   例如 voice_inject_cmd(VOICE_CMD_QUERY_TEMP) 让模块自己播报，
     *   或者按 voice_seq[] 的约定下发一个字节（如 0x13 = query_temp），
     *   由模块的语音合成播报预置好的语句。业务层调用点不用改。 */
    return voice_speak(text);
}

esp_err_t voice_inject_cmd(voice_cmd_t cmd)
{
    /* 参数校验后走【和真实模块完全相同】的 dispatch 通道 */
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 注意：注入【不】改变 online 标志 —— 在线与否只反映"真实模块有没有发字节"，
     * 这样拔掉模块后 voice_is_online() 仍会正确变成 false。 */
    voice_dispatch(cmd);
    return ESP_OK;
}

const char *voice_cmd_name(voice_cmd_t cmd)
{
    if ((int)cmd < 0 || cmd >= VOICE_CMD_MAX) {
        return name_table[VOICE_CMD_NONE];      /* 越界统一返回 "none" */
    }
    return name_table[cmd];
}

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

bool voice_is_online(void)
{
    portENTER_CRITICAL(&s_state_mux);
    const bool    online  = s_online;
    const int64_t last_us = s_last_rx_us;
    portEXIT_CRITICAL(&s_state_mux);

    if (!online) {
        return false;
    }

    /* 超过 30s 没收到任何字节 → 认为模块被拔掉/断电了，自动回到离线 */
    if ((esp_timer_get_time() - last_us) > VOICE_ONLINE_TIMEOUT_US) {
        portENTER_CRITICAL(&s_state_mux);
        s_online = false;
        portEXIT_CRITICAL(&s_state_mux);
        ESP_LOGW(TAG, "no data for 30s, voice module considered offline");
        return false;
    }

    return true;
}
