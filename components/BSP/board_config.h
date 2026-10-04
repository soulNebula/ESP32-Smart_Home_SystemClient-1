/**
 * @file    board_config.h
 * @brief   ESP32-S3 智能家居 —— 硬件引脚分配表【全工程唯一真源 / Single Source of Truth】
 *
 * 目标板 : ESP32-S3-DevKitC-1
 *          模组 ESP32-S3-WROOM-1-N16R8（16MB Quad Flash + 8MB Octal PSRAM）
 * 框架   : ESP-IDF v5.4.x
 *
 * ===========================================================================
 *  ★ 要改引脚，只改本文件。全工程都从这里取值，任何地方都不许硬编码 GPIO 号。
 * ===========================================================================
 *
 *  【禁用 / 保留 GPIO —— 绝对不要接外设】
 *  ┌────────────┬────────────────────────────────────────────────────────────┐
 *  │ GPIO 26–32 │ 模组内部 SPI Flash 专用（也没引出到排针）                    │
 *  │ GPIO 33–37 │ N16R8 的 Octal PSRAM 占用。33/34 未引出；35/36/37 引出了但   │
 *  │            │ 已被 PSRAM 吃掉，接外设会导致 PSRAM 读写错乱、随机死机        │
 *  │ GPIO 19/20 │ USB D- / D+。接外设会导致 USB 无法枚举                        │
 *  │ GPIO 43/44 │ UART0 控制台（日志输出 + 串口烧录）                           │
 *  │ GPIO 0     │ 启动 strapping（低电平进下载模式）                            │
 *  │ GPIO 45    │ 启动 strapping（VDD_SPI 电压选择），上电必须为低或悬空         │
 *  │ GPIO 46    │ 启动 strapping（内部下拉），上电不可被拉高                     │
 *  │ GPIO 3     │ 启动 strapping（JTAG 源选择），本方案直接不用                  │
 *  │ GPIO 38    │ DevKitC-1 v1.0 的板载 RGB 灯 → 不要接外设                     │
 *  │ GPIO 48    │ DevKitC-1 v1.1 的板载 RGB 灯 → 本工程用作【状态指示灯】        │
 *  │            │ （v1.0 板子的状态灯在 GPIO38，那种板子上状态灯不亮；要支持  │
 *  │            │  v1.0 就把下面的 BSP_STATUS_LED_GPIO 改成 GPIO_NUM_38）     │
 *  └────────────┴────────────────────────────────────────────────────────────┘
 *
 *  【已分配 GPIO 总表】← 接线就照这张表接
 *  ┌──────┬────────────────────┬─────────────┬──────────────────────────────────┐
 *  │ GPIO │ 功能               │ 外设/模式    │ 接到哪里                          │
 *  ├──────┼────────────────────┼─────────────┼──────────────────────────────────┤
 *  │   8  │ I2C SDA            │ I2C0 400kHz │ OLED / SHT30 / BH1750 的 SDA      │
 *  │   9  │ I2C SCL            │ I2C0 400kHz │ OLED / SHT30 / BH1750 的 SCL      │
 *  │   1  │ 光照模拟量         │ ADC1_CH0    │ 光敏电阻分压中点（见接线指南）      │
 *  │   2  │ 雨滴模拟量         │ ADC1_CH1    │ 雨滴模块 AO                       │
 *  │  12  │ 雨滴数字量(可选)    │ GPIO 输入   │ 雨滴模块 DO                       │
 *  │   4  │ WS2812 客厅        │ RMT TX0     │ 客厅灯带 DIN                      │
 *  │   5  │ WS2812 厨房        │ RMT TX1     │ 厨房灯带 DIN                      │
 *  │   6  │ WS2812 卧室        │ RMT TX2     │ 卧室灯带 DIN                      │
 *  │   7  │ WS2812 浴室        │ RMT TX3     │ 浴室灯带 DIN                      │
 *  │  10  │ 用户按键 KEY1      │ GPIO 输入   │ 按键一脚→GPIO10，另一脚→GND        │
 *  │  11  │ 用户按键 KEY2      │ GPIO 输入   │ 同上                              │
 *  │  14  │ 风扇测速 TACH(可选) │ PCNT        │ 4 线风扇第 3 脚（黄线）            │
 *  │  18  │ 风扇 PWM           │ LEDC 25kHz  │ MOS 管栅极（见接线指南）           │
 *  │  15  │ 舵机 窗帘          │ LEDC 50Hz   │ 窗帘舵机信号线（橙）               │
 *  │  16  │ 舵机 窗户          │ LEDC 50Hz   │ 窗户舵机信号线（橙）               │
 *  │  17  │ 舵机 门            │ LEDC 50Hz   │ 门舵机信号线（橙）                 │
 *  │  21  │ 语音模块→ESP32     │ UART1 RX    │ 语音模块 TXD                      │
 *  │  47  │ ESP32→语音模块     │ UART1 TX    │ 语音模块 RXD                      │
 *  │  48  │ 板载 RGB 状态灯    │ 位带驱动     │ 板载，不用接线（v1.1）             │
 *  │  13  │ INMP441 I²S SCK    │ I2S0 BCLK   │ INMP441 SCK（位时钟）★不是I²C SCL │
 *  │  39  │ INMP441 I²S WS     │ I2S0 LRCLK  │ INMP441 WS（帧同步）★不是I²C      │
 *  │  40  │ INMP441 I²S SD     │ I2S0 DIN    │ INMP441 SD（数据）★不是I²C SDA    │
 *  │  12  │ 雨滴 DO（可选）     │ GPIO 输入   │ 雨滴模块 DO（实际没接，可让给其它）│
 *  │ 41/42│ 预留扩展           │ —           │ 以后加传感器用                     │
 *  └──────┴────────────────────┴─────────────┴──────────────────────────────────┘
 *
 *  【⚠ INMP441 是 I²S，不是 I²C —— 最容易接错的地方】
 *    INMP441 是数字音频麦克风，走 I²S 三线制（SCK/WS/SD），没有 I²C 从机地址，
 *    【绝对不能接在 GPIO8/GPIO9 的 SDA/SCL 上】。完整六根线见下面第 10 节。
 *
 *  【⚠ 供电警告 —— 最容易翻车的地方】
 *   3 个舵机 + 风扇的电流远超开发板 3V3/5V 引脚的输出能力。
 *   SG90 堵转约 700mA/个，MG996R 堵转可达 2.5A/个；ESP32 板载 LDO 只能给几百 mA。
 *   → 舵机/风扇必须用【独立 5V 电源】（如 5V/3A 适配器或 DC-DC 降压模块），
 *     并且【独立电源的 GND 必须和开发板 GND 接在一起】（共地），否则 PWM 信号无效。
 *   → WS2812 灯带同理：每颗灯珠满亮约 60mA，60 颗全白就是 3.6A，务必外接电源。
 *   细节见 docs/04-接线与供电指南.md
 */
#ifndef __BOARD_CONFIG_H__
#define __BOARD_CONFIG_H__

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/*  1. I2C 总线（OLED + SHT30 + BH1750 共用一条）                             */
/* ========================================================================= */
#define BSP_I2C_PORT            I2C_NUM_0
#define BSP_I2C_SDA_GPIO        GPIO_NUM_8
#define BSP_I2C_SCL_GPIO        GPIO_NUM_9
#define BSP_I2C_FREQ_HZ         400000      /* 400kHz 快速模式 */
#define BSP_I2C_TIMEOUT_MS      100

/* I2C 从机地址（买不同模块时如果读不到数据，先扫 I2C 确认地址） */
#define BSP_I2C_ADDR_SSD1306    0x3C        /* OLED 128x64，有的模块是 0x3D */
#define BSP_I2C_ADDR_SHT30      0x44        /* SHT30 / SHT31 */
#define BSP_I2C_ADDR_AHT20      0x38        /* AHT20（备选） */
#define BSP_I2C_ADDR_BH1750     0x23        /* BH1750（备选光照） */
#define BSP_I2C_ADDR_PCF8574    0x20        /* 预留：I2C 扩展 IO */

/* ========================================================================= */
/*  2. ADC 模拟输入（ESP32-S3 只有 ADC1/ADC2；WiFi 开启后 ADC2 不可用 → 全用 ADC1） */
/* ========================================================================= */
#define BSP_ADC_UNIT            ADC_UNIT_1
#define BSP_ADC_ATTEN           ADC_ATTEN_DB_12   /* 量程约 0 ~ 3100mV */
#define BSP_ADC_BITWIDTH        ADC_BITWIDTH_12   /* 0 ~ 4095 */

#define BSP_ADC_CH_LIGHT        ADC_CHANNEL_0     /* GPIO1  光敏电阻 */
#define BSP_ADC_CH_RAIN         ADC_CHANNEL_1     /* GPIO2  雨滴 AO */

/* 上面两个 ADC 通道对应的物理引脚（ADC_CHANNEL_n 的数值不是 GPIO 号，别混用） */
#define BSP_GPIO_LIGHT_ADC      GPIO_NUM_1        /* = ADC1_CH0 */
#define BSP_GPIO_RAIN_AO        GPIO_NUM_2        /* = ADC1_CH1 */

#define BSP_GPIO_RAIN_DO        GPIO_NUM_12       /* 雨滴模块 DO，可选 */

/* ---------------------------------------------------------------------------
 *  光敏模块 AO 的【极性】—— 硬件事实，换模块才需要改
 * ---------------------------------------------------------------------------
 *  0 = 亮 → AO 电压【高】   （模块内 VCC—光敏—AO—电阻—GND 这种接法）
 *  1 = 亮 → AO 电压【低】   （模块内 VCC—电阻—AO—光敏—GND）
 *
 *  ★ 实机判定（2026-09-29）：把房间灯全部关掉时，status 里 L 反而显示 100%
 *    （AO 电压顶到 ADC 满量程），说明本模块是【反极性】→ 这里设 1。
 *
 *  怎么自查：串口敲 status，看 "L:xx%(NNNNmV)"：
 *      遮住光敏 / 关灯 → 应接近 0%（mV 低）；强光照射 → 应接近 100%（mV 高）。
 *      若正好相反，把这个宏改一下重新编译即可。
 *  极性搞反的后果（本次实际踩到）：黑暗中读数=100% → 自动联动"误以为很亮"
 *      → 把刚打开的客厅灯又关掉、把窗帘拉开。
 */
#define BSP_LIGHT_ADC_INVERT    1

/* ========================================================================= */
/*  3. WS2812 灯带（4 个房间各一路，共 4 路 = ESP32-S3 全部 4 个 RMT TX 通道） */
/* ========================================================================= */
#define BSP_WS2812_RMT_RES_HZ   10000000    /* 10MHz → 1 tick = 0.1us */

/* ★ 必须 ≤ SOC_RMT_MEM_WORDS_PER_CHANNEL（ESP32-S3 = 48），且为偶数。
 *   给大了（比如 64）驱动会向上取整成"每个通道占 2 个内存块"，
 *   而 4 个 RMT TX 通道总共只有 4 块内存 → 第 3 路灯带
 *   rmt_new_tx_channel() 会直接返回 ESP_ERR_NOT_FOUND，灯带建不起来。
 *   48 个 symbol ≈ 57.6us，驱动会在半块处中断续传，对本应用完全够用。 */
#define BSP_WS2812_MEM_BLOCK    (48)

#define BSP_WS2812_GPIO_LIVING  GPIO_NUM_4  /* 客厅 */
#define BSP_WS2812_GPIO_KITCHEN GPIO_NUM_5  /* 厨房 */
#define BSP_WS2812_GPIO_BEDROOM GPIO_NUM_6  /* 卧室 */
#define BSP_WS2812_GPIO_BATH    GPIO_NUM_7  /* 浴室 */

/* 每条灯带的灯珠数量 —— 按你实际买的灯带改这里 */
#define BSP_WS2812_LED_NUM_LIVING   30
#define BSP_WS2812_LED_NUM_KITCHEN  30
#define BSP_WS2812_LED_NUM_BEDROOM  30
#define BSP_WS2812_LED_NUM_BATH     30

/* ========================================================================= */
/*  3b. 灯的驱动方式（二选一）—— 引脚完全相同，只换驱动方式                  */
/* ========================================================================= */
/*  1 = WS2812 / WS2812B 可寻址灯带（单总线 RMT 时序，支持【颜色】+ 亮度）
 *  0 = 普通单色 LED 模块（每路一个 GPIO，LEDC PWM 调亮度，【没有颜色】）
 *
 *  ★ 当前设为 0，因为实际采购到的是「电子积木 5mm LED 发光模块」（单色白光），
 *    不是 WS2812 灯带。详见 docs/07-元器件采购订单.md 的采购核对。
 *
 *  以后换成 WS2812B 灯带时，把这里改回 1 即可，其它代码一行都不用动。
 */
#define BSP_LED_BACKEND_WS2812  0

/* ---- 下面这些只在 BSP_LED_BACKEND_WS2812 = 0 时生效 ----
 * LEDC 定时器 0 被舵机占用、1 被风扇占用 → 灯用定时器 2；
 * 通道 0/1/2 是舵机、3 是风扇 → 灯用通道 4~7。 */
#define BSP_LED_PLAIN_TIMER     LEDC_TIMER_2
#define BSP_LED_PLAIN_MODE      LEDC_LOW_SPEED_MODE
#define BSP_LED_PLAIN_RES       LEDC_TIMER_8_BIT      /* 0~255 */
#define BSP_LED_PLAIN_FREQ_HZ   5000                  /* 5kHz：无可见频闪、无啸叫 */
#define BSP_LED_PLAIN_DUTY_MAX  255
/* 多数"电子积木 LED 模块"是【高电平点亮】；若你的模块是低电平点亮，改成 1 */
#define BSP_LED_PLAIN_ACTIVE_LOW 0

/* ========================================================================= */
/*  4. 舵机（窗帘 / 窗户 / 门）—— LEDC 硬件 PWM，50Hz                     */
/*     注意：三路共用同一个 LEDC 定时器（50Hz 必须一致），占 3 个通道         */
/* ========================================================================= */
#define BSP_SERVO_TIMER         LEDC_TIMER_0
#define BSP_SERVO_MODE          LEDC_LOW_SPEED_MODE
#define BSP_SERVO_RES           LEDC_TIMER_14_BIT   /* 14bit @50Hz → 1 计数 ≈ 1.22us */
#define BSP_SERVO_FREQ_HZ       50

#define BSP_SERVO_CH_CURTAIN    LEDC_CHANNEL_0
#define BSP_SERVO_CH_WINDOW     LEDC_CHANNEL_1
#define BSP_SERVO_CH_DOOR       LEDC_CHANNEL_2

#define BSP_SERVO_GPIO_CURTAIN  GPIO_NUM_15
#define BSP_SERVO_GPIO_WINDOW   GPIO_NUM_16
#define BSP_SERVO_GPIO_DOOR     GPIO_NUM_17

/* 舵机脉宽范围：SG90 约 500~2500us，MG996R 约 500~2500us
 * 如果舵机转到头会"滋滋"响或抖动，把 MAX 调小一点（例如 2400） */
#define BSP_SERVO_MIN_PULSE_US  500
#define BSP_SERVO_MAX_PULSE_US  2500

/* 舵机到位后自动松劲的延时（毫秒）：
 * 到位后保持力矩会顶着机械止点较劲 → 高频抖动/嗡鸣，还费电。
 * 松劲 = 停止输出 PWM，舵机失去力矩。轻负载（窗帘/窗户/门）松劲后一般
 * 不会跑位；如果你的机构松劲后会自己滑落（比如窗帘自动卷回），把这里改成 0
 * 关闭自动松劲，改为把上面 MAX 脉宽调小避免顶止点。 */
#define BSP_SERVO_RELEASE_MS     1000

/* ========================================================================= */
/*  5. 风扇 —— MOS 管 PWM 调速（LEDC 25kHz，超出人耳听觉）                   */
/* ========================================================================= */
#define BSP_FAN_TIMER           LEDC_TIMER_1
#define BSP_FAN_MODE            LEDC_LOW_SPEED_MODE
#define BSP_FAN_CHANNEL         LEDC_CHANNEL_3
#define BSP_FAN_RES             LEDC_TIMER_10_BIT   /* 0 ~ 1023 */
#define BSP_FAN_FREQ_HZ         25000
#define BSP_FAN_GPIO_PWM        GPIO_NUM_18
#define BSP_FAN_GPIO_TACH       GPIO_NUM_14         /* 测速，可选，不接也能跑 */
#define BSP_FAN_TACH_ENABLE     0                   /* 1=启用测速，0=不用 */

/* ========================================================================= */
/*  6. 按键（KEY1 / KEY2）—— 按下接地，内部上拉，低电平有效                  */
/*  ★ 2026-09-28：IO10 已让给外接的【五位 AD 键盘】（ADC1_CH9），KEY1 弃用   */
/*    （设 GPIO_NUM_NC = 不配置/不扫描）；五位键盘驱动见 ADKEY/adkey.c。     */
/* ========================================================================= */
#define BSP_KEY_GPIO_KEY1       GPIO_NUM_NC         /* 原 GPIO10，现给五位 AD 键盘 */
#define BSP_KEY_GPIO_KEY2       GPIO_NUM_11
#define BSP_KEY_ACTIVE_LEVEL    0                   /* 0 = 按下为低电平 */
#define BSP_KEY_DEBOUNCE_MS     30
#define BSP_KEY_LONG_PRESS_MS   2000                /* 长按判定 */
#define BSP_KEY_COUNT           2

/* ========================================================================= */
/*  7. 语音模块（UART1）—— 本轮只留接口，硬件到了直接接                        */
/*     兼容：SU-03T / ASRPRO / LD3320 / 天问 等"串口命令词"模块              */
/* ========================================================================= */
#define BSP_VOICE_UART_PORT     UART_NUM_1
#define BSP_VOICE_TX_GPIO       GPIO_NUM_47         /* ESP32 TX → 模块 RXD */
#define BSP_VOICE_RX_GPIO       GPIO_NUM_21         /* ESP32 RX ← 模块 TXD */
#define BSP_VOICE_BAUD          9600                /* 多数语音模块默认 9600 */
#define BSP_VOICE_RX_BUF_SIZE   1024

/* ========================================================================= */
/*  8. 板载 RGB 状态灯（WS2812，GPIO48）
 *     为什么不用 RMT：ESP32-S3 只有 4 个 RMT TX 通道，已经被 4 路灯带占满。
 *     状态灯刷新频率很低（状态变化时才刷），用位带（关中断 ~30us）完全够。
 * ========================================================================= */
#define BSP_STATUS_LED_ENABLE   1
#define BSP_STATUS_LED_GPIO     GPIO_NUM_48

/* ========================================================================= */
/*  9. INMP441 I²S 数字麦克风（离线语音识别 ESP-SR 的拾音前端）
 *     ★★ 注意：INMP441 是 I²S，【不是 I²C】★★
 *     INMP441 没有 I²C 从机地址，不能挂在 GPIO8/GPIO9 的 SDA/SCL 上；
 *     它是 I²S 三线制（位时钟 / 帧同步 / 数据），靠 WS 电平区分左右声道。
 *
 *     【完整六根线】—— 插不上就照这张表数扩展板丝印
 *       INMP441 SCK (位时钟) ── GPIO13   扩展板左排 IO13
 *       INMP441 WS  (帧同步) ── GPIO39   扩展板右排 IO39
 *       INMP441 SD  (数据)   ── GPIO40   扩展板右排 IO40
 *       INMP441 L/R (声道)   ── GND      ★ 单麦必须固定接一端，悬空=左右乱跳/无声
 *       INMP441 VDD          ── 3.3V     ★ 绝对不要接 5V（INMP441 是 3.3V 器件）
 *       INMP441 GND          ── GND
 *
 *     【为什么选这三个脚】
 *       · GPIO13/39/40 都在扩展板丝印上（左排 13；右排 39/40），接线不用数针脚；
 *       · 不与本工程任何已定义功能冲突：GPIO38 是 DevKitC-1 v1.0 的板载状态灯，
 *         工程文档明确"别占"；GPIO12 预留给"雨滴 DO"，GPIO14 预留给"风扇测速 TACH"，
 *         这两个虽然当前没接，但保留原用途更省心；
 *       · 代价：GPIO39~42 是 USB-JTAG 脚，占用后【内置 JTAG 调试失效】。
 *         本工程用 UART 桥（GPIO43/44）做控制台和烧录，JTAG 本来就没用，可接受。
 *       · 若你的板子没引出 IO39/IO40：把 WS 换 GPIO12、SD 换 GPIO14 也行，
 *         但要先确认雨滴 DO 和风扇测速确实不接，改这里重新编译即可。
 *
 *     采样格式固定 16KHz / 16bit / 单声道 —— 这是 ESP-SR 对输入的硬性要求，
 *     所以不在这里暴露成可配置项，避免上层配错导致"能出声但识别不了"。
 * ========================================================================= */
#define BSP_I2S_MIC_SCK_GPIO    GPIO_NUM_13         /* I²S BCLK 位时钟 → INMP441 SCK */
#define BSP_I2S_MIC_WS_GPIO     GPIO_NUM_39         /* I²S LRCLK 帧同步 → INMP441 WS */
#define BSP_I2S_MIC_SD_GPIO     GPIO_NUM_40         /* I²S DIN 数据     → INMP441 SD */
#define BSP_I2S_MIC_SAMPLE_RATE 16000               /* ESP-SR 要求 16KHz，不要改 */
#define BSP_I2S_MIC_ENABLE      1                   /* 1 = 上电就初始化 I²S 麦克风 */

/* ========================================================================= */
/*  10. 业务常量（默认值，运行时可被 MQTT / 语音 / 按键覆盖）                  */
/* ========================================================================= */

/* 灯带分区 —— 同时也是 MQTT topic 里的名字，改这里要同步改 mqtt_protocol.h */
#define BSP_LED_ZONE_COUNT      4

/* 舵机编号 */
#define BSP_SERVO_COUNT         3

/* 默认阈值（真正生效的值在 automation 模块里，可被 MQTT 改） */
#define BSP_DEF_LIGHT_ON_LUX    50      /* 光照低于 50lux  → 开灯/拉窗帘 */
#define BSP_DEF_LIGHT_OFF_LUX   200     /* 光照高于 200lux → 关灯/开窗帘（迟滞） */
#define BSP_DEF_TEMP_FAN_ON_C   28      /* 温度高于 28°C   → 开风扇 */
#define BSP_DEF_TEMP_FAN_OFF_C  26      /* 温度低于 26°C   → 关风扇（迟滞） */
#define BSP_DEF_RAIN_PCT        30      /* 雨滴湿度 > 30%  → 判定下雨，关窗 */

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_CONFIG_H__ */
