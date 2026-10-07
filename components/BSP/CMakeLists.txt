set(BSP_EXTRA_REQUIRES "")

# 读配置文件看开关
set(_bsp_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig")
if(EXISTS "${_bsp_sdkconfig}")
    file(STRINGS "${_bsp_sdkconfig}" _bsp_voice_sr_on
         REGEX "^CONFIG_APP_VOICE_SOURCE_ESP_SR=y")
    if(_bsp_voice_sr_on)
        list(APPEND BSP_EXTRA_REQUIRES espressif__esp-sr json)
        message(STATUS "BSP: 语音识别来源 = ESP-SR，已 REQUIRES espressif__esp-sr + json")
    else()
        message(STATUS "BSP: 语音识别来源 = ASRPRO（UART），不依赖 esp-sr")
    endif()
else()
    message(STATUS "BSP: 找不到 sdkconfig (${_bsp_sdkconfig})，按不启用 ESP-SR 处理")
endif()

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
        ${BSP_EXTRA_REQUIRES}
)
