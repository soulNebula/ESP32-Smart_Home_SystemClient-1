# astra-ui 组件（AstraThreshold/oled-ui-astra v0.0.2-alpha，C++ OLED UI 框架）
# 目录层级保持与原项目一致（astra/ + hal/），ESP32 适配：
# - hal/esp32/astra_hal_esp32 为本项目新增的 HAL 实现
# - 画布用 u8g2 全缓冲纯软件实例渲染，发送由 main 组件的 OLED 驱动完成
idf_component_register(
    SRCS
        "astra/astra_rocket.cpp"
        "astra/ui/launcher.cpp"
        "astra/ui/element/page/item.cpp"
        "hal/hal.cpp"
        "hal/esp32/astra_hal_esp32.cpp"
    INCLUDE_DIRS
        "."
        "astra"
        "hal"
        "hal/esp32"
    REQUIRES
        u8g2
        driver
        esp_timer
)
