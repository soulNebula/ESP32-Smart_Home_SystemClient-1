#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把整块板子准备好
esp_err_t board_init(void);

// 取出总线把手
i2c_master_bus_handle_t board_get_i2c_bus(void);

// 打印引脚对照表
void board_print_pinmap(void);

#ifdef __cplusplus
}
#endif
