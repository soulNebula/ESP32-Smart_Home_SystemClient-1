#pragma once

// 板级配置要用 Serial / millis 那一套，模块里 include 这份就都带上了
#include <Arduino.h>

// ======================================================================
// 板级配置：引脚、参数、默认阈值
//
// 这份是从 ESP-IDF 工程的 components/BSP/board_config.h 搬过来的，
// 数值一个没改，只是引脚不再写 GPIO_NUM_x，直接写数字（Arduino 就认数字）。
// 改接线只改这一份。
// ======================================================================

// ---- I2C：屏幕和温湿度共用一条 ----
// 数据脚
#define BSP_I2C_SDA_GPIO        8
// 时钟脚
#define BSP_I2C_SCL_GPIO        9
// 跑400k够快
#define BSP_I2C_FREQ_HZ         400000

// 各个芯片的地址
// 屏幕，有的0x3D
#define BSP_I2C_ADDR_SSD1306    0x3C
// 温湿度芯片
#define BSP_I2C_ADDR_AHT20      0x38
// 备用温湿度
#define BSP_I2C_ADDR_SHT30      0x44

// ---- ADC：只读光敏和雨滴两路 ----
// 光照那一路
#define BSP_GPIO_LIGHT_ADC      1
// 雨滴那一路
#define BSP_GPIO_RAIN_AO        2
// 雨滴开关量，可选，不接也行
#define BSP_GPIO_RAIN_DO        12
// 亮的定义反了就改成 1
#define BSP_LIGHT_ADC_INVERT    1

// ---- 四路房间灯：普通单色 LED 模块，LEDC 调亮度 ----
// 客厅灯脚
#define BSP_LED_GPIO_LIVING     4
// 厨房灯脚
#define BSP_LED_GPIO_KITCHEN    5
// 卧室灯脚
#define BSP_LED_GPIO_BEDROOM    6
// 浴室灯脚
#define BSP_LED_GPIO_BATH       7

// 灯的调光参数
// 不闪也不响
#define BSP_LED_FREQ_HZ         5000
// 八位够用
#define BSP_LED_RES_BITS        8
// 1=低电平点亮
#define BSP_LED_ACTIVE_LOW      0

// ---- LEDC 通道分配，照原工程排的，别乱改 ----
// 舵机占 0~2，风扇占 3，四路灯占 4~7
#define BSP_SERVO_CH_CURTAIN    0
#define BSP_SERVO_CH_WINDOW     1
#define BSP_SERVO_CH_DOOR       2
#define BSP_FAN_CHANNEL         3
#define BSP_LED_CH_LIVING       4
#define BSP_LED_CH_KITCHEN      5
#define BSP_LED_CH_BEDROOM      6
#define BSP_LED_CH_BATH         7

// ---- 三路舵机共用一套 PWM ----
// 一路 LEDC 定时器带三路
#define BSP_SERVO_FREQ_HZ       50
// 十四位，一格约 1.2 微秒
#define BSP_SERVO_RES_BITS      14

// 窗帘那路舵机不要了，这里写死停用：1=接，0=不接
// 停用后 GPIO15 上什么都不出，窗帘命令一律当不支持
#define BSP_SERVO_CURTAIN_ENABLE 0

// 窗帘信号脚（上面写死停用后这个脚空着不接）
#define BSP_SERVO_GPIO_CURTAIN  15
// 窗户信号脚
#define BSP_SERVO_GPIO_WINDOW   16
// 门信号脚
#define BSP_SERVO_GPIO_DOOR     17

// 脉宽范围，抖就调小
#define BSP_SERVO_MIN_PULSE_US  500
#define BSP_SERVO_MAX_PULSE_US  2500

// 窗户和门两路舵机的关位、开位角度（度）
// 关停在三十度，开走到一百五十度，两头都不顶死
#define BSP_SERVO_WINDOW_DOOR_CLOSED_DEG   30
#define BSP_SERVO_WINDOW_DOOR_OPEN_DEG     150

// 到位后松劲免得响，0 就是一直使劲
#define BSP_SERVO_RELEASE_MS     1000

// ---- 风扇：低边 MOS 管，PWM 调速 ----
// 风扇调速脚
#define BSP_FAN_GPIO_PWM        18
// 25kHz 超出人耳听觉，不会有啸叫
#define BSP_FAN_FREQ_HZ         25000
// 十位，满值 1023
#define BSP_FAN_RES_BITS        10

// ---- 按键：KEY2 按下接地 ----
// KEY1 的脚让给键盘了
#define BSP_KEY_GPIO_KEY1       (-1)
// 第二路按键脚
#define BSP_KEY_GPIO_KEY2       11
// 0=按下是低
#define BSP_KEY_ACTIVE_LEVEL    0
// 防抖时间
#define BSP_KEY_DEBOUNCE_MS     30
// 长按判定
#define BSP_KEY_LONG_PRESS_MS   2000

// ---- 五位 AD 键盘：一路 ADC 认五个键 ----
#define BSP_ADKEY_GPIO          10
// 每个键的分压点，单位毫伏
#define BSP_ADKEY_KEY1_MV       1397
#define BSP_ADKEY_KEY2_MV       2639
#define BSP_ADKEY_KEY3_MV       627
#define BSP_ADKEY_KEY4_MV       1971
// 认键的容差
#define BSP_ADKEY_TOLERANCE_MV  200
// OK 键是低压档，低于这条就算 OK
#define BSP_ADKEY_OK_LOW_MAX_MV 300
// 空闲时电压要高于这条，低了说明有键粘住
#define BSP_ADKEY_IDLE_MIN_MV   3000
// 消抖和长按
#define BSP_ADKEY_DEBOUNCE_MS   30
#define BSP_ADKEY_LONG_PRESS_MS 2000

// ---- 板载状态灯：WS2812 一颗 ----
#define BSP_STATUS_LED_ENABLE   1
// v1.0 板子改成 38
#define BSP_STATUS_LED_GPIO     48

// ---- 语音模块 ASRPRO：串口那条路，交叉接 ----
// ESP32 收
#define BSP_VOICE_UART_RX_GPIO  21
// ESP32 发
#define BSP_VOICE_UART_TX_GPIO  47
// 模块默认就是这个波特率
#define BSP_VOICE_UART_BAUD     9600

// ---- 开机默认阈值，跑起来能在 App 或串口里改 ----
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

// 灯分四区，跟上报里的名字走
#define BSP_LED_ZONE_COUNT      4

// 传感器采样周期，主循环按这个节拍采
#define BSP_SENSOR_PERIOD_MS    500

// 屏幕刷新周期，别刷太勤
#define BSP_OLED_PERIOD_MS      200
