/*
 * 模块：
 *   屏幕的对外接口。只负责屏幕这件事：把硬件准备好、探一下屏在不在、
 *   清屏、整屏刷新、按页写入。被 main.c 和 astra UI 调用，
 *   向下调 i2c_bus 读写屏幕芯片；画面由 astra UI 画好再交给它。
 *
 * 功能：
 *   把硬件准备好
 *   没屏就当没这回事
 *   整屏推上去显示
 *   按页把画面推上去
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH   128
#define OLED_HEIGHT  64

/* 功能：把硬件准备好 */
esp_err_t oled_init(void);

/* 功能：屏幕能用吗 */
bool oled_is_ready(void);

/* 功能：清空显示 */
void oled_clear(void);

/* 功能：整屏推上去 */
void oled_refresh(void);

/* 功能：写一页并上屏 */
void oled_write_page(uint8_t page, const uint8_t *data);

#ifdef __cplusplus
}
#endif
