/**
 * @file  board.h
 * @brief 板级初始化入口 —— 把所有硬件外设按正确顺序拉起来
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化整块板子的硬件（I2C → ADC → 灯带 → 舵机 → 风扇 → 传感器 → OLED）
 *
 * 幂等，可重复调用。单个外设初始化失败不会中断流程：
 * 例如传感器还没插上，只会打警告日志并把该外设标记为不可用，
 * 其余功能照常运行 —— 方便"配件还没到就先跑起来"。
 *
 * @return 全部成功返回 ESP_OK；部分失败返回 ESP_ERR_NOT_FOUND（并打印清单）
 */
esp_err_t board_init(void);

/** @brief 取全局 I2C 总线句柄（board_init 之后可用，未初始化返回 NULL） */
i2c_master_bus_handle_t board_get_i2c_bus(void);

/** @brief 把 board_config.h 里的引脚表打到日志里，方便接线时对照 */
void board_print_pinmap(void);

#ifdef __cplusplus
}
#endif
