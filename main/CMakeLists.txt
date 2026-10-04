# 模块：
#   主程序。把 main.c 和 astra_glue.cpp 编进来，
#   并说明要用 BSP、App、astra_ui、u8g2 这几个组件。
#
# 功能：
#   登记源文件
#   声明依赖

idf_component_register(
    SRCS "main.c" "astra_glue.cpp"
    INCLUDE_DIRS "."
    REQUIRES BSP App driver nvs_flash esp_wifi astra_ui u8g2
)
