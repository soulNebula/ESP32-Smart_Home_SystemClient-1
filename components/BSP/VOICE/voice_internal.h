/*
 * 模块：
 *   语音的内部小接口。只给同目录下的实现文件用，外面的模块请只 include voice.h。
 *   两条语音来源（串口模块、麦克风离线识别）都从这里的一个口子出去，
 *   汇到 voice.c 再叫上层的 voice_on_cmd。
 *
 * 能做这些：
 *   把一条命令交给上层
 *
 * 几句说明：
 *   这个口子是故意不放到 voice.h 里的。一旦公开，上层就可能绕过
 *   "自己造一条命令"那个入口直接乱调，两条来源走一个出口的约定就破了。
 *   C 里没有包的概念，只能靠命名和注释约定：外面请只 include voice.h。
 *   回话口跑在谁的任务里，取决于这话是谁送上来的 —— 麦克风识别送来的
 *   就跑在识别的任务里，串口来的就跑在收串口的任务里，自己注入的
 *   就跑在调用者的任务里。所以回话口里别做费时的活，耗时的动作
 *   请转投业务队列。
 */
#pragma once

#include "voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：把命令交出去 */
void voice_dispatch(voice_cmd_t cmd);

#ifdef __cplusplus
}
#endif
