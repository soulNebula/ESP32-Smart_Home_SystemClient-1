/*
 * 模块：
 *   开机自检。挨个点一遍设备看灵不灵：灯亮不亮、风扇转不转、窗门帘
 *   动不动，最后看一眼传感器的读数。界面在屏幕上画，键盘管上下翻和
 *   确认；被 main.c 在自检时调用，自己向下调设备总状态表
 *   （device_model.c）动手，自动联动（automation.c）由它临时摁住。
 *
 *   进法：OK 键长按两秒左右，或者快点三下（也能在串口敲 test）。
 *   键位：1 退出，3 上一项，4 下一项，OK 单击执行当前项。
 *   一共九项：客厅灯、厨房灯、卧室灯、浴室灯、风扇、窗户、门、窗帘、
 *   传感器。自检期间自动联动先关掉，退出时恢复原样，不写进 flash。
 */
#pragma once

#include "adkey.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：现在是不是在自检 */
bool selftest_is_active(void);

/* 功能：进自检，先摁住联动 */
void selftest_enter(void);

/* 功能：退自检，把联动放回去 */
void selftest_exit(void);

/* 功能：键盘按键进来 */
void selftest_on_adkey(adkey_id_t id, adkey_event_t ev);

/* 功能：每半秒推进一下状态 */
void selftest_tick(void);

/* 功能：执行当前这一项 */
void selftest_run_current(void);

/* 功能：翻到下一项 */
void selftest_next(void);

/* 功能：翻到上一项 */
void selftest_prev(void);

#ifdef __cplusplus
}
#endif

/* 功能：一共几项 */
uint8_t selftest_get_count(void);

/* 功能：现在选中的是第几项 */
uint8_t selftest_get_index(void);

/* 功能：当前项处于哪一步 */
uint8_t selftest_get_phase(void);

/* 功能：某一项的名字 */
const char *selftest_get_item_name(uint8_t idx);

/* 功能：直接跳到第几项并开跑 */
void selftest_run_index(uint8_t idx);
