# 模块：
#   硬件驱动层（板级支持包）。板子初始化、按键、灯、屏幕、传感器、语音
#   全在这里，向上给 App 和 main 用，向下直接操作芯片外设。
#
#   ESP-IDF 只把 components/ 下"带 CMakeLists.txt 的直接子目录"当组件，
#   所以 BSP 只算一个组件，KEY/ LED/ I2C/ 都只是它的子目录，
#   在这里统一登记源文件和头文件目录就行。
#
#   语音识别要用的 esp-sr 是【有条件】的依赖：只有打开
#   「语音识别来源 = ESP-SR」时才真的用到它。
#   这里的判断方式是【直接读 sdkconfig 文件】，而不是用 CMake 变量——
#   踩过的坑：组件展开阶段会 configure 两次，第一次 configure 时
#   sdkconfig.cmake 还没生成，所有 CONFIG_* 都是空的，连一直是 y 的
#   开关也判成空，于是 esp-sr 进不了依赖表，后面再 configure 也不会补。
#   报错是 esp_afe_config.h 找不到，看着像名字写错，其实是没进去。
#   读文件则任何一次 configure 都稳。
#   组件名必须写 espressif__esp-sr（组件管理器按 命名空间__名字 注册），
#   不能写成 esp-sr。json 是 IDF 自带的 cJSON，esp-sr 打包模型脚本要用。
#
# 功能：
#   登记源文件
#   声明依赖
#   按开关加 esp-sr

set(BSP_EXTRA_REQUIRES "")

# 功能：读配置文件看开关
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
