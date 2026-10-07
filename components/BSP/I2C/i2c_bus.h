#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 起总线，重复调没事
esp_err_t i2c_bus_init(void);

// 没起就返回空
i2c_master_bus_handle_t i2c_bus_get_handle(void);

// 看地址在不在线
esp_err_t i2c_bus_probe(uint8_t dev_addr);

// 扫一遍谁在线
int i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif
