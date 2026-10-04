/*
 * 模块：
 *   界面和业务的中间层。给 main.c 一个口子，调用它就能起界面，
 *   所以被 main.c 调用；自己向下用 astra_rocket 搭界面、
 *   用 device_model 读设备状态。具体写法在 astra_glue.cpp。
 *
 * 功能：
 *   起界面任务
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：起界面任务 */
void astra_ui_start(void);

#ifdef __cplusplus
}
#endif
