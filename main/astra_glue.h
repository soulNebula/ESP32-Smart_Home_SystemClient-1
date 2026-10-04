/**
 * @file  astra_glue.h
 * @brief astra UI 与智能家居业务之间的胶水层（C 入口，实现见 astra_glue.cpp）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 astra UI 任务（OLED 渲染 + 五键导航由该任务全权负责） */
void astra_ui_start(void);

#ifdef __cplusplus
}
#endif
