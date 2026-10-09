#pragma once

#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// 把整块板子准备好
esp_err_t board_init(void);

// 打印引脚对照表
void board_print_pinmap(void);

#ifdef __cplusplus
}
#endif
