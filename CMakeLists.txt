#  所以本工程实际有 4 个组件：
#    components/BSP        → BSP        所有硬件驱动（引脚/总线/外设）
#    components/App        → App        业务逻辑、联动规则、WiFi、MQTT
#    components/astra_ui   → astra_ui   OLED 菜单框架（C++，含 u8g2 画布 HAL）
#    components/u8g2       → u8g2       图形库

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_smart_home)
