#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH   128
#define OLED_HEIGHT  64

// 把硬件准备好
esp_err_t oled_init(void);

// 屏幕能用吗
bool oled_is_ready(void);

// 清空显示
void oled_clear(void);

// 整屏推上去
void oled_refresh(void);

// 写一页并上屏
void oled_write_page(uint8_t page, const uint8_t *data);

#ifdef __cplusplus
}
#endif
