# CMakeLists.txt for BSP component
# 语音识别来源已固定为板载 INMP441(I²S) + ESP-SR 离线识别，
# 所以 espressif__esp-sr 是固定依赖，不再按 sdkconfig 开关判断

idf_component_register(
    SRCS
        "board.c"
        "KEY/key.c"
        "ADKEY/adkey.c"
        "LED/led.c"
        "I2C/i2c_bus.c"
        "ADC/adc_bus.c"
        "WS2812/ws2812.c"
        "SERVO/servo.c"
        "FAN/fan.c"
        "SENSOR/sensor.c"
        "OLED/oled.c"
        "VOICE/voice.c"
        "VOICE/voice_esp_sr.c"
        "I2S_MIC/i2s_mic.c"
    INCLUDE_DIRS
        "."
        "KEY"
        "ADKEY"
        "LED"
        "I2C"
        "ADC"
        "WS2812"
        "SERVO"
        "FAN"
        "SENSOR"
        "OLED"
        "VOICE"
        "I2S_MIC"
    REQUIRES
        driver
        esp_adc
        esp_timer
        esp_ringbuf
        espressif__esp-sr
        json
)
