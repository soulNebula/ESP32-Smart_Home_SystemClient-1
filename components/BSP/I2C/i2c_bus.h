/*
 * 模块：
 *   I2C 总线的对外口子。屏幕、温湿度、光照都靠它读写，
 *   被 oled.c 和 sensor.c 调用，开机时 board.c 先起它，
 *   具体实现在 i2c_bus.c。
 *
 * 功能：
 *   起总线
 *   交出总线把手
 *   探测某个地址
 *   扫一遍线上有啥
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：起总线，重复调没事 */
esp_err_t i2c_bus_init(void);

/* 功能：没起就返回空 */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/* 功能：看地址在不在线 */
esp_err_t i2c_bus_probe(uint8_t dev_addr);

/* 功能：扫一遍谁在线 */
int i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif
