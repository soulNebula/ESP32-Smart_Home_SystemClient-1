#pragma once

#include <stdbool.h>
#include <stdint.h>

// ======================================================================
// 自检：五键键盘点着往下走，一项一项验
//
// 对应 ESP-IDF 工程的 components/App/selftest.c。
// 自检期间自动联动先摁住，走完再恢复。
// ======================================================================

// 一项的三步
typedef enum {
    // 等着按
    SELFTEST_PHASE_IDLE = 0,
    // 正在跑
    SELFTEST_PHASE_RUN,
    // 跑完了
    SELFTEST_PHASE_DONE,
} selftest_phase_t;

bool selftest_is_active(void);

// 进自检模式，顺手把联动摁住
void selftest_enter(void);

// 退出自检，把设备收好，联动恢复原样
void selftest_exit(void);

// 跑当前这一项
void selftest_run_current(void);

// 切下一项
void selftest_next(void);

// 主循环喊这个，计时收尾
void selftest_tick(void);

uint8_t selftest_get_count(void);
uint8_t selftest_get_index(void);
uint8_t selftest_get_phase(void);

// 这一项屏幕上显示的名字
const char *selftest_get_item_name(uint8_t idx);

// 直接跳去跑第 idx 项
void selftest_run_index(uint8_t idx);
