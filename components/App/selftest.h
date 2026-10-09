#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 现在是不是在自检
bool selftest_is_active(void);

// 进自检，先摁住联动
void selftest_enter(void);

// 退自检，把联动放回去
void selftest_exit(void);

// 每半秒推进一下状态
void selftest_tick(void);

// 执行当前这一项
void selftest_run_current(void);

// 翻到下一项
void selftest_next(void);

#ifdef __cplusplus
}
#endif

// 一共几项
uint8_t selftest_get_count(void);

// 现在选中的是第几项
uint8_t selftest_get_index(void);

// 当前项处于哪一步
uint8_t selftest_get_phase(void);

// 某一项的名字
const char *selftest_get_item_name(uint8_t idx);

// 直接跳到第几项并开跑
void selftest_run_index(uint8_t idx);
