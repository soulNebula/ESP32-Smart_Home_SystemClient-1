# 模块：
#   工程总入口，被 ESP-IDF 的构建系统读。本工程分四个组件：
#   BSP 管硬件、App 管业务、astra_ui 管界面、u8g2 是画图库。
#
# 功能：
#   定工程名
#   加载 IDF

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_smart_home)
