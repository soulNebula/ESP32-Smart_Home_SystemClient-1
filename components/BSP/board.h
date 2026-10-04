/*
 * 模块：
 *   板级开机的对外口子。main.c 只要调这几个函数，整块板子的硬件就都起来了，
 *   所以被 main.c 调用；具体怎么起在 board.c，引脚都在 board_config.h。
 *
 * 功能：
 *   把整块板子准备好
 *   取出总线把手
 *   打印引脚对照表
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：把整块板子准备好 */
esp_err_t board_init(void);

/* 功能：取出总线把手 */
i2c_master_bus_handle_t board_get_i2c_bus(void);

/* 功能：打印引脚对照表 */
void board_print_pinmap(void);

#ifdef __cplusplus
}
#endif
