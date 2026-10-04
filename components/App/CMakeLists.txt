# ============================================================================
#  App —— 业务逻辑层（设备模型 / 自动联动 / WiFi / MQTT）
#  不直接碰寄存器，所有硬件操作都通过 BSP 暴露的接口。
# ============================================================================

idf_component_register(
    SRCS
        "device_model.c"
        "automation.c"
        "wifi_sta.c"
        "mqtt_app.c"
        "mqtt_protocol.c"
        "app_link.c"        # 传输链路注册表（MQTT / BLE 共用）
                            # ★ app_cmd.h 只有声明，实现放在 mqtt_app.c 里，故不需要 app_cmd.c
        "ble_app.c"         # BLE (NimBLE) GATT 服务端：手机 App 的第二条控制链路
        "selftest.c"        # 按键自检模式（OLED 显示测试项，按键切换/执行）
    INCLUDE_DIRS
        "."
    REQUIRES
        BSP
        driver
        esp_wifi
        esp_netif
        esp_event
        mqtt            # esp-mqtt 客户端
        json            # cJSON
        nvs_flash
        esp_timer
        bt              # NimBLE 协议栈（ble_app.c 依赖；BT_ENABLED=n 时该组件不导出
                        #  NimBLE 头文件，ble_app.c 内部有 #if 兜底，不会链接失败）
)
