/**
 * @file  selftest.h
 * @brief 按键自检模式：OLED 显示测试项名称与内容，按键切换/执行
 *
 * 进入方式：OK 长按约 2 秒，或 OK 快速连按 3 次（或串口敲 `test`）
 * 自检模式内：
 *   3 = 上一项，4 = 下一项
 *   OK 单击 = 执行当前测试项
 *   1 = 退出自检模式（OK 键长按不可靠，见 main.c 注释）
 *
 * 自检期间自动联动被临时关闭（退出后恢复原状态，不写 NVS）。
 * 测试项共 9 个：客厅灯/厨房灯/卧室灯/浴室灯/风扇/窗户/门/窗帘/传感器。
 * OLED 布局：大号 "TEST n/9" + 16x16 中文项目名 + 状态（等待/检测中/完成）
 *           + 按键提示行；传感器项直接显示温湿度/光照/雨滴读数。
 */
#pragma once

#include "adkey.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 是否处于自检模式 */
bool selftest_is_active(void);

/** 进入自检模式（暂停自动联动，记录原状态） */
void selftest_enter(void);

/** 退出自检模式（恢复自动联动原状态） */
void selftest_exit(void);

/** 五位键盘事件入口：仅在自检模式内调用（main.c 里先判断 is_active）
 *  键位：3 = 上一项，4 = 下一项，OK 单击 = 执行，OK 长按 = 退出 */
void selftest_on_adkey(adkey_id_t id, adkey_event_t ev);

/** 自检状态机推进 + OLED 刷新：由 app_loop 每 500ms 调一次 */
void selftest_tick(void);

/** 执行当前项（供串口 `test run` 调用） */
void selftest_run_current(void);

/** 切到下一项（供串口 `test next` 调用） */
void selftest_next(void);

/** 切到上一项（键 3 调用） */
void selftest_prev(void);

#ifdef __cplusplus
}
#endif

/* ---------------- 状态查询（astra UI 页面每帧刷新用） ---------------- */

/** 测试项总数 */
uint8_t selftest_get_count(void);

/** 当前选中项（0 ~ count-1） */
uint8_t selftest_get_index(void);

/** 当前项状态：0=等待 1=检测中 2=完成 */
uint8_t selftest_get_phase(void);

/** 指定项名称（字库内中文名）；越界返回空串 */
const char *selftest_get_item_name(uint8_t idx);

/** 直接执行第 idx 项（设置当前项并开始；供 UI 页面的 OK 回调调用） */
void selftest_run_index(uint8_t idx);
