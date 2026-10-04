idf_component_register(
    SRCS "main.c" "astra_glue.cpp"
    INCLUDE_DIRS "."
    REQUIRES BSP App driver nvs_flash esp_wifi astra_ui u8g2
)
