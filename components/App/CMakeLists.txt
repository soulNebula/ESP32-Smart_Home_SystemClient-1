# 模块：
#   App 业务层。管设备状态、自动联动、WiFi、MQTT、蓝牙和自检，
#   自己不碰硬件，硬件的活都交给 BSP 组件。
#
# 功能：
#   登记源文件
#   声明依赖

idf_component_register(
    SRCS
        "device_model.c"
        "automation.c"
        "wifi_sta.c"
        "mqtt_app.c"
        "mqtt_protocol.c"
        "app_link.c"
        "ble_app.c"
        "selftest.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        BSP
        driver
        esp_wifi
        esp_netif
        esp_event
        mqtt
        json
        nvs_flash
        esp_timer
        bt
)
