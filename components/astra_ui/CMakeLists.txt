# 模块：
#   界面组件（AstraThreshold/oled-ui-astra，C++ 写的 OLED 菜单框架）。
#   目录保持 astra/ 和 hal/ 两层，其中 hal/esp32/ 是本项目自己加的适配层。
#   画面先由 u8g2 在内存里画好，再交给 BSP 的 OLED 驱动发到屏幕。
#
# 功能：
#   登记源文件
#   声明依赖

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
