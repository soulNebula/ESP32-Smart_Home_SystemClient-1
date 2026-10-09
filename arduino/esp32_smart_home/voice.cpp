// ======================================================================
// 语音：ASRPRO 模块走串口上报命令
//
// 对应 ESP-IDF 工程 components/BSP/VOICE/voice.c 里串口那条路
// （原版还有 INMP441 + ESP-SR 离线识别那条路，那是乐鑫的闭源库，
//  Arduino 里没有，所以这版只走 ASRPRO 串口，模块自己认字）。
//
// 模块发过来的东西两种都认：
//   一、原版的帧：一个不可打印字节就是命令号，0x01 数下来查 s_seq_table
//       0x01=led_living_on ... 0x19=auto_off
//   二、纯文本命令名：整行给过来，如 "fan_on"、"led_kitchen_off"，
//       行尾 \n 或 \r\n。天问Block 里常把 ASRPRO 配成直接发命令名字符串，
//       所以这条也得认，不然模块一换配置就全哑了。
//       0x20 到 0x7E 的字才算文本，所以空格能夹在行里（行首行尾的会擦掉），
//       制表符 0x09 之类的不算，它们走上面那条帧的路。
//
// 收字节在 voice_poll() 里干，主循环喊，一次只啃几个字节就返回，绝不等。
// ======================================================================

#include <Arduino.h>
#include <stddef.h>
#include <string.h>

#include "app_config.h"
#include "voice.h"

#if APP_VOICE_ENABLE

// 日志前缀，跟原版 TAG = "voice" 对上
#define VOICE_LOG_PREFIX    "voice: "
// 一行最多攒多少字节（跟原版 VOICE_LINE_MAX 一样），攒不下就整行丢掉
#define VOICE_LINE_MAX      64
// 主循环里一次最多啃几个字节，剩下的下一轮再说，别占着主循环不放
// 9600 波特一秒才来 960 字节，一轮 8 个足够了
#define VOICE_POLL_MAX      8

// UART1 给语音模块，交叉接：ESP32 RX=GPIO21 / TX=GPIO47
static HardwareSerial voiceSerial(1);

// 命令词的对照表，字符串必须和原工程 voice.c 一字不差，
// 串口命令台的 say / help-voice 都指着它
static const char *const s_name_table[VOICE_CMD_MAX] = {
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

// 表长对不上就是加了命令忘了加名字
static_assert(sizeof(s_name_table) / sizeof(s_name_table[0]) == VOICE_CMD_MAX,
              "s_name_table[] 条目数必须等于 VOICE_CMD_MAX（与 voice.h 枚举顺序一一对应）");

// 单字节帧的对照表：0x01 排第一条，跟原版 voice_seq[] 一个顺序
static const voice_cmd_t s_seq_table[] = {
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

// 少一条就有命令号认不出来
static_assert(sizeof(s_seq_table) / sizeof(s_seq_table[0]) == VOICE_CMD_MAX - 1,
              "s_seq_table[] 必须覆盖 VOICE_CMD_NONE 之后的全部指令（VOICE_CMD_MAX - 1 条）");

// 上层留的回话口
static voice_cmd_cb_t s_cb = NULL;
static void          *s_cb_user = NULL;
// 开过没，防重开
static bool           s_inited = false;

// 攒一行文字
static char   s_line[VOICE_LINE_MAX];
static size_t s_line_len = 0;
// 这行已经太长废掉了没，废了就只等行尾把它清干净
static bool   s_line_over = false;

// 字节翻命令
static voice_cmd_t voice_seq_lookup(uint8_t byte) {
    // 0x01 排第一条
    const int idx = (int)byte - 1;

    if (idx < 0 || idx >= (int)(sizeof(s_seq_table) / sizeof(s_seq_table[0]))) {
        // 0x00 和 0x1A 往后都在表外
        return VOICE_CMD_NONE;
    }
    return s_seq_table[idx];
}

// 擦掉行首行尾的空格
// 天问Block 里配字符串容易手滑带上空格，顺手擦了省得认不出来
// 只擦空格：制表符是 0x09，那属于单字节帧（0x09 正好是 led_all_on），
// 根本攒不进行里，用不着管
static void voice_trim_line(char *line, size_t *len) {
    size_t n    = *len;
    size_t head = 0;

    // 数出前头有几个空格
    for (head = 0; head < n; head++) {
        if (line[head] != ' ') {
            break;
        }
    }
    // 尾巴上的也削掉
    for (; n > head; n--) {
        if (line[n - 1] != ' ') {
            break;
        }
    }

    if (head > 0) {
        memmove(line, line + head, n - head);
    }
    line[n - head] = '\0';
    *len = n - head;
}

// 所有语音都从这走
static void voice_dispatch_cmd(voice_cmd_t cmd) {
    if (cmd <= VOICE_CMD_NONE || cmd >= VOICE_CMD_MAX) {
        // 没认出来就丢掉
        return;
    }

    // 命令号和名字都打出来，好对着日志查
    Serial.printf(VOICE_LOG_PREFIX "voice cmd: %s (id=%d)\n",
                  voice_cmd_name(cmd), (int)cmd);

    if (s_cb != NULL) {
        s_cb(cmd, s_cb_user);
    }
}

// 逐字节认，原版也是这么一条一条啃的
static void voice_parse_byte(uint8_t byte) {
    // 不是能读的字、又不是行尾的，当单字节命令号（原版 ASRPRO 的帧就这么发）
    if (byte != '\r' && byte != '\n' && (byte < 0x20 || byte > 0x7E)) {
        // 半截文字不要了
        s_line_len  = 0;
        s_line_over = false;

        const voice_cmd_t cmd = voice_seq_lookup(byte);
        if (cmd == VOICE_CMD_NONE) {
            Serial.printf(VOICE_LOG_PREFIX "unknown voice seq byte 0x%02X, dropped\n",
                          (unsigned)byte);
        }
        voice_dispatch_cmd(cmd);
        return;
    }

    if (byte == '\r' || byte == '\n') {
        // 模块发的是整行，攒够一行再认
        if (s_line_over) {
            // 太长那行整条扔掉，从下一行重来
            Serial.printf(VOICE_LOG_PREFIX "line too long (max %d), dropped\n",
                          VOICE_LINE_MAX - 1);
            s_line_over = false;
            s_line_len  = 0;
            return;
        }

        if (s_line_len > 0) {
            voice_trim_line(s_line, &s_line_len);
            // 纯文本命令名翻命令号
            const voice_cmd_t cmd = voice_cmd_from_name(s_line);
            if (cmd == VOICE_CMD_NONE && s_line_len > 0) {
                Serial.printf(VOICE_LOG_PREFIX "unknown voice text line: \"%s\", dropped\n",
                              s_line);
            }
            voice_dispatch_cmd(cmd);
            s_line_len = 0;
        }
        // 空行跳过（\r\n 的第二个字节走到这就是这种）
        return;
    }

    // 已经废掉的行不用再往里塞
    if (s_line_over) {
        return;
    }

    if (s_line_len < VOICE_LINE_MAX - 1) {
        s_line[s_line_len++] = (char)byte;
    } else {
        // 塞不下了，这行作废，等行尾来清
        s_line_over = true;
        s_line_len  = 0;
    }
}

// 开机把串口准备好
bool voice_init(void) {
    if (s_inited) {
        // 第二次直接走
        return true;
    }

    // 9600 8N1，跟模块出厂设置对上；模块没接也不会报错
    voiceSerial.begin(BSP_VOICE_UART_BAUD, SERIAL_8N1,
                      BSP_VOICE_UART_RX_GPIO, BSP_VOICE_UART_TX_GPIO);

    // 从头开始攒行
    s_line_len  = 0;
    s_line_over = false;
    s_inited    = true;

    Serial.printf(VOICE_LOG_PREFIX "init ok (语音识别来源 = 外部 ASRPRO 模块，"
                                   "UART1 baud=%d rx=GPIO%d tx=GPIO%d)\n",
                  BSP_VOICE_UART_BAUD, BSP_VOICE_UART_RX_GPIO, BSP_VOICE_UART_TX_GPIO);
    Serial.printf(VOICE_LOG_PREFIX "等模块报命令行（单字节帧、命令名两种都认）；"
                                   "模块没接也不影响别的功能，先用 voice_inject_cmd() 跑业务\n");
    return true;
}

// 登记上层的回话口
void voice_register_cb(voice_cmd_cb_t cb, void *user_data) {
    // 传 NULL 就是把回话口撤了
    s_cb      = cb;
    s_cb_user = user_data;
}

// 主循环喊这个，收模块发来的行
void voice_poll(void) {
    if (!s_inited) {
        return;
    }

    // 只手动 available()/read()，不用 readBytes，所以不会在这等超时
    // 一次啃够 VOICE_POLL_MAX 个字节就撒手，剩下的下一轮再说
    for (int i = 0; i < VOICE_POLL_MAX; i++) {
        if (voiceSerial.available() <= 0) {
            break;
        }

        const int b = voiceSerial.read();
        if (b < 0) {
            break;
        }
        voice_parse_byte((uint8_t)b);
    }
}

// 自己造一条命令，串口 say 和自检都走它
void voice_inject_cmd(voice_cmd_t cmd) {
    // 和真模块同一路：注入不算在线，只走回调
    voice_dispatch_cmd(cmd);
}

// 命令号翻名字
const char *voice_cmd_name(voice_cmd_t cmd) {
    if ((int)cmd < 0 || cmd >= VOICE_CMD_MAX) {
        // 越界都给 none
        return s_name_table[VOICE_CMD_NONE];
    }
    return s_name_table[cmd];
}

// 名字翻回命令号，找不到给 VOICE_CMD_NONE
voice_cmd_t voice_cmd_from_name(const char *name) {
    if (name == NULL) {
        return VOICE_CMD_NONE;
    }

    // 模块发来的字符串就在这跟表里的对上
    for (int i = 0; i < VOICE_CMD_MAX; i++) {
        if (strcmp(name, s_name_table[i]) == 0) {
            return (voice_cmd_t)i;
        }
    }
    // 认不出来就当没说
    return VOICE_CMD_NONE;
}

#else  // APP_VOICE_ENABLE

// 语音关掉了：口子全留着，谁调都不崩，只是啥也不干

// 没开就直说
bool voice_init(void) {
    return false;
}

void voice_register_cb(voice_cmd_cb_t cb, void *user_data) {
    (void)cb;
    (void)user_data;
}

void voice_poll(void) {
}

void voice_inject_cmd(voice_cmd_t cmd) {
    (void)cmd;
}

const char *voice_cmd_name(voice_cmd_t cmd) {
    (void)cmd;
    // 关了也得给个能打印的串
    return "none";
}

voice_cmd_t voice_cmd_from_name(const char *name) {
    (void)name;
    // 关了也得给 VOICE_CMD_NONE，不然串口 say 会拿到野值
    return VOICE_CMD_NONE;
}

#endif  // APP_VOICE_ENABLE
