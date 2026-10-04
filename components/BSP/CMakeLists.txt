# ============================================================================
#  BSP —— 板级支持包
#
#  注意：ESP-IDF 只会把 components/ 下"带 CMakeLists.txt 的直接子目录"当成组件。
#  所以 BSP 是【一个】组件，KEY/ LED/ I2C/ 等只是它的源码子目录，不需要各自的
#  CMakeLists.txt —— 在这里统一登记 SRCS 和 INCLUDE_DIRS 即可。
#
#  ★ 关于 esp-sr 的依赖是【有条件】的：
#    voice_esp_sr.c 只有在 menuconfig 打开
#    「语音识别来源使用 ESP-SR」时才真正用到 esp-sr 的头文件和库。
#    所以这里用 ${BSP_EXTRA_REQUIRES} 把 esp-sr 挂成条件依赖：
#      · 开关【关】（默认）→ REQUIRES 里没有 esp-sr
#        → 即使 managed_components/ 里没有 esp-sr，本工程照样能一次编译通过
#        （保持"无网环境下也能 build"的老原则）；
#      · 开关【开】→ 需要 espressif__esp-sr / json
#        （json 是 IDF 内置的 cJSON 组件，esp-sr 的模型打包脚本要用）。
#    这样"关掉开关就完全回到 ASRPRO UART 方案"这句话才是真的。
#
#  ⚠ 注意 esp-sr 的组件名是 espressif__esp-sr，不是 esp-sr —— 见下面第 21 行注释。
# ============================================================================

# ============================================================================
#  ★ esp-sr 依赖为什么要这么绕（本工程实测踩过，记录一下）
# ----------------------------------------------------------------------------
#  想当然的写法是：
#        if(CONFIG_APP_VOICE_SOURCE_ESP_SR)
#            list(APPEND BSP_EXTRA_REQUIRES espressif__esp-sr)
#        endif()
#
#  但它【不可靠】，实测结果：
#    ESP-IDF 组件展开阶段会 configure 两次。第一次 configure 时
#    sdkconfig.cmake 还没来得及生成，所有 CONFIG_* 变量都是空字符串
#    —— 连一直是 y 的 CONFIG_APP_SERIAL_DEBUG_ENABLE 也是空的。
#    于是那次 if 判为假，esp-sr 没进 REQUIRES，而组件属性一旦定下来，
#    后面再 configure 也不会补上。症状非常误导：
#        fatal error: esp_afe_config.h: No such file or directory
#        "esp_afe_config.h ... is provided by espressif__esp-sr component(s).
#         However, espressif__esp-sr is not in the requirements list of BSP."
#    看起来像"名字写错了"，其实是"那次判断根本没进去"。
#
#  所以这里【直接读 sdkconfig 文件】判断，不看 CMake 变量，任何一次
#  configure 都稳定。sdkconfig 在工程根目录（= 本文件的上两级）。
# ============================================================================

set(BSP_EXTRA_REQUIRES "")

set(_bsp_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig")
if(EXISTS "${_bsp_sdkconfig}")
    file(STRINGS "${_bsp_sdkconfig}" _bsp_voice_sr_on
         REGEX "^CONFIG_APP_VOICE_SOURCE_ESP_SR=y")
    if(_bsp_voice_sr_on)
        # ★ 组件名必须是 espressif__esp-sr（不是 esp-sr）！
        #   组件管理器把在线组件按 <命名空间>__<名字> 注册成组件，
        #   managed_components/espressif__esp-sr 这个目录名就是它的组件名。
        #   json = IDF 内置的 cJSON 组件（esp-sr 的模型打包脚本要用）。
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
        driver          # 汇总组件：gpio / i2c / ledc / rmt / uart / i2s / gptimer / pcnt
        esp_adc         # adc_oneshot + adc_cali 校准
        esp_timer       # 微秒级延时（舵机平滑转动用）
        esp_ringbuf
        ${BSP_EXTRA_REQUIRES}
)
