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

// 屏幕温湿度共用一条
#define BSP_I2C_PORT            I2C_NUM_0
// 数据脚
#define BSP_I2C_SDA_GPIO        GPIO_NUM_8
// 时钟脚
#define BSP_I2C_SCL_GPIO        GPIO_NUM_9
// 跑400k够快
#define BSP_I2C_FREQ_HZ         400000
#define BSP_I2C_TIMEOUT_MS      100

// 各个芯片的地址
// 屏幕，有的0x3D
#define BSP_I2C_ADDR_SSD1306    0x3C
// 温湿度芯片
#define BSP_I2C_ADDR_SHT30      0x44
// 备用温湿度
#define BSP_I2C_ADDR_AHT20      0x38
// 备用光照
#define BSP_I2C_ADDR_BH1750     0x23
// 预留扩IO
#define BSP_I2C_ADDR_PCF8574    0x20

// 只用ADC1，WiFi一开ADC2就废
#define BSP_ADC_UNIT            ADC_UNIT_1
// 量程约3.1V
#define BSP_ADC_ATTEN           ADC_ATTEN_DB_12
// 满值4095
#define BSP_ADC_BITWIDTH        ADC_BITWIDTH_12

// 光照那一路
#define BSP_ADC_CH_LIGHT        ADC_CHANNEL_0
// 雨滴那一路
#define BSP_ADC_CH_RAIN         ADC_CHANNEL_1

// 通道号对应的脚，别混
// 光照脚
#define BSP_GPIO_LIGHT_ADC      GPIO_NUM_1
// 雨滴脚
#define BSP_GPIO_RAIN_AO        GPIO_NUM_2

// 雨滴开关量，可选
#define BSP_GPIO_RAIN_DO        GPIO_NUM_12

// 亮的定义反了就改它
// 1=亮时电压低
#define BSP_LIGHT_ADC_INVERT    1

// 四路灯带的公共参数
// 一格0.1微秒
#define BSP_WS2812_RMT_RES_HZ   10000000

// 改大了就少一路灯
#define BSP_WS2812_MEM_BLOCK    (48)

// 客厅灯带脚
#define BSP_WS2812_GPIO_LIVING  GPIO_NUM_4
// 厨房灯带脚
#define BSP_WS2812_GPIO_KITCHEN GPIO_NUM_5
// 卧室灯带脚
#define BSP_WS2812_GPIO_BEDROOM GPIO_NUM_6
// 浴室灯带脚
#define BSP_WS2812_GPIO_BATH    GPIO_NUM_7

// 每路的灯珠数，按实际改
#define BSP_WS2812_LED_NUM_LIVING   30
#define BSP_WS2812_LED_NUM_KITCHEN  30
#define BSP_WS2812_LED_NUM_BEDROOM  30
#define BSP_WS2812_LED_NUM_BATH     30

// 1=灯带，0=普通灯
#define BSP_LED_BACKEND_WS2812  0

// 普通灯的调光参数
#define BSP_LED_PLAIN_TIMER     LEDC_TIMER_2
#define BSP_LED_PLAIN_MODE      LEDC_LOW_SPEED_MODE
// 满值255
#define BSP_LED_PLAIN_RES       LEDC_TIMER_8_BIT
// 不闪也不响
#define BSP_LED_PLAIN_FREQ_HZ   5000
#define BSP_LED_PLAIN_DUTY_MAX  255
// 1=低电平点亮
#define BSP_LED_PLAIN_ACTIVE_LOW 0

// 三路舵机共用一套
#define BSP_SERVO_TIMER         LEDC_TIMER_0
#define BSP_SERVO_MODE          LEDC_LOW_SPEED_MODE
// 一格约1.2微秒
#define BSP_SERVO_RES           LEDC_TIMER_14_BIT
#define BSP_SERVO_FREQ_HZ       50

#define BSP_SERVO_CH_CURTAIN    LEDC_CHANNEL_0
#define BSP_SERVO_CH_WINDOW     LEDC_CHANNEL_1
#define BSP_SERVO_CH_DOOR       LEDC_CHANNEL_2

// 窗帘信号脚
#define BSP_SERVO_GPIO_CURTAIN  GPIO_NUM_15
// 窗户信号脚
#define BSP_SERVO_GPIO_WINDOW   GPIO_NUM_16
// 门信号脚
#define BSP_SERVO_GPIO_DOOR     GPIO_NUM_17

// 脉宽范围，抖就调小
#define BSP_SERVO_MIN_PULSE_US  500
#define BSP_SERVO_MAX_PULSE_US  2500

// 到位后松劲免得响
#define BSP_SERVO_RELEASE_MS     1000

// 风扇调速的参数
#define BSP_FAN_TIMER           LEDC_TIMER_1
#define BSP_FAN_MODE            LEDC_LOW_SPEED_MODE
#define BSP_FAN_CHANNEL         LEDC_CHANNEL_3
// 满值1023
#define BSP_FAN_RES             LEDC_TIMER_10_BIT
#define BSP_FAN_FREQ_HZ         25000
// 风扇调速脚
#define BSP_FAN_GPIO_PWM        GPIO_NUM_18
// 测速脚，可不接
#define BSP_FAN_GPIO_TACH       GPIO_NUM_14
// 0=不用测速
#define BSP_FAN_TACH_ENABLE     0

// 按键，按下接地
// 脚让给键盘了
#define BSP_KEY_GPIO_KEY1       GPIO_NUM_NC
// 第二路按键脚
#define BSP_KEY_GPIO_KEY2       GPIO_NUM_11
// 0=按下是低
#define BSP_KEY_ACTIVE_LEVEL    0
// 防抖时间
#define BSP_KEY_DEBOUNCE_MS     30
// 长按判定
#define BSP_KEY_LONG_PRESS_MS   2000
#define BSP_KEY_COUNT           2

// 语音模块的串口脚
#define BSP_VOICE_UART_PORT     UART_NUM_1
// 发给模块
#define BSP_VOICE_TX_GPIO       GPIO_NUM_47
// 收模块的话
#define BSP_VOICE_RX_GPIO       GPIO_NUM_21
// 多数模块9600
#define BSP_VOICE_BAUD          9600
#define BSP_VOICE_RX_BUF_SIZE   1024

// 板上那颗小灯
#define BSP_STATUS_LED_ENABLE   1
// v1.0板子改成38
#define BSP_STATUS_LED_GPIO     GPIO_NUM_48

// 麦克风，是I2S不是I2C
// 位时钟脚
#define BSP_I2S_MIC_SCK_GPIO    GPIO_NUM_13
// 帧同步脚
#define BSP_I2S_MIC_WS_GPIO     GPIO_NUM_39
// 数据脚
#define BSP_I2S_MIC_SD_GPIO     GPIO_NUM_40
// 认字要16K
#define BSP_I2S_MIC_SAMPLE_RATE 16000
// 开机就起麦克风
#define BSP_I2S_MIC_ENABLE      1

// 开机默认值，跑起来能改

// 灯分四区，跟topic走
#define BSP_LED_ZONE_COUNT      4

// 舵机有三路
#define BSP_SERVO_COUNT         3

// 联动的默认门槛
// 暗过50就开灯
#define BSP_DEF_LIGHT_ON_LUX    50
// 亮过200就关灯
#define BSP_DEF_LIGHT_OFF_LUX   200
// 热过28开风扇
#define BSP_DEF_TEMP_FAN_ON_C   28
// 凉到26关风扇
#define BSP_DEF_TEMP_FAN_OFF_C  26
// 湿过30算下雨
#define BSP_DEF_RAIN_PCT        30

#ifdef __cplusplus
}
#endif

// 配置表到此结束
#endif
