#pragma once

#include <Arduino.h>

// ======================================================================
// 版本兼容层：ESP32 的 Arduino 核心包 2.x 和 3.x 接口不一样
//
// PWM（LEDC）：
//   2.x：先 ledcSetup(通道, 频率, 位数) 再 ledcAttachPin(脚, 通道)，写按通道号
//   3.x：ledcAttach(脚, 频率, 位数) 一步到位，写按脚
// 板载那颗 WS2812：
//   2.x 叫 neopixelWrite，3.x 叫 rgbLedWrite
//
// 甲方机器上装的是哪个版本的核心包都行，这里替他们兜住。
// ======================================================================

// 3.x 才有这个宏，2.x 里当 0 算，正好走老接口
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define PWM_CORE_V3 1
#else
#define PWM_CORE_V3 0
#endif

// 把一路 PWM 挂到脚上，成功给 true
static inline bool pwm_attach(uint8_t ch, uint8_t pin, uint32_t freq_hz, uint8_t res_bits) {
#if PWM_CORE_V3
    // 3.x 自己挑通道，ch 只是留着对照原工程的排法
    (void)ch;
    return ledcAttach(pin, freq_hz, res_bits);
#else
    ledcSetup(ch, freq_hz, res_bits);
    ledcAttachPin(pin, ch);
    return true;
#endif
}

// 改占空比
static inline void pwm_write(uint8_t ch, uint8_t pin, uint32_t duty) {
#if PWM_CORE_V3
    (void)ch;
    ledcWrite(pin, duty);
#else
    (void)pin;
    ledcWrite(ch, duty);
#endif
}

// 板载状态灯写个颜色
static inline void status_led_write(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
#if PWM_CORE_V3
    rgbLedWrite(pin, r, g, b);
#else
    neopixelWrite(pin, r, g, b);
#endif
}
