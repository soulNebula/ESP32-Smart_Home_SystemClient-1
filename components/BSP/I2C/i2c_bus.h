/**
 * @file  i2c_bus.h
 * @brief I2C0 总线管理 —— OLED / SHT30 / AHT20 / BH1750 共用
 *
 * 使用 ESP-IDF v5.4 的新版 I2C Master 驱动（driver/i2c_master.h），
 * 不是已废弃的 driver/i2c.h 老 API。
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 I2C0（SDA=GPIO8, SCL=GPIO9, 400kHz），幂等 */
esp_err_t i2c_bus_init(void);

/** @brief 取 I2C 总线句柄；未初始化返回 NULL */
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/**
 * @brief 探测某个从机地址是否在线
 * @return ESP_OK=在线；ESP_ERR_NOT_FOUND=无应答；其它=总线错误
 */
esp_err_t i2c_bus_probe(uint8_t dev_addr);

/**
 * @brief 扫描 0x08~0x77 全部地址并把结果打到日志
 * @return 扫描到的设备数量
 *
 * 传感器读不到数据时，先看这条日志确认地址对不对。
 */
int i2c_bus_scan(void);

#ifdef __cplusplus
}
#endif
