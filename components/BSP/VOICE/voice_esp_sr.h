/*
 * 模块：
 *   语音的另一条来源：用一颗麦克风加乐鑫的离线识别，先喊唤醒词再说命令。
 *   它被 voice.c 调用，自己向下调 i2s_mic.c 拾音、调 esp-sr 认话，
 *   认出来的命令交回语音模块（voice.c 里的下发口），
 *   最后送到 main.c 的 voice_on_cmd，和串口模块那条路完全一样。
 *
 * 能做这些：
 *   开机把识别跑起来
 *   看识别好了没
 *   报当前唤醒词
 *   报命令词模型
 *   打印命令词表
 *   看是不是在等命令
 *   往屏幕递一句话
 *
 * 几句说明：
 *   这块默认是关的，开关在 menuconfig 的"语音识别来源"里。关掉的时候
 *   实现文件整段被条件编译切掉，工程回到串口模块那条老路，行为不变。
 *   开起来之后由这边接管拾音、认出命令，再交回同一个出口，
 *   上层一行都不用改。
 *   本方案只有麦克风、没有喇叭。控制类的命令完全正常，灯会真亮；
 *   查询类的命令会正常走完业务链（屏幕会显示、上报也会发生），
 *   但不会出声 —— 这不是毛病，是本来就没有喇叭。
 *   将来要说话，得另加一路功放加喇叭，本任务不涉及。
 *   屏幕提示是识别任务只负责写文字，界面任务自己来取。不能直接在
 *   识别任务里弹窗，弹窗动画是个阻塞循环，会把实时识别拖垮。
 *   唤醒词的名字是从模型自带的信息里读出来的，所以它一定和实际烧进
 *   flash 的模型对得上 —— 用户必须知道要喊哪个词，这个函数就是答案的来源。
 *   命令词表是给串口调试台用的，照着念就能测，不用翻代码。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：把识别跑起来 */
esp_err_t voice_esp_sr_start(void);

/* 功能：看识别起来没 */
bool voice_esp_sr_is_ready(void);

/* 功能：报要喊那句话 */
const char *voice_esp_sr_wake_word(void);

/* 功能：报命令词模型 */
const char *voice_esp_sr_mn_model(void);

/* 功能：打一张命令表 */
void voice_esp_sr_print_commands(void);

/* 功能：看在不在等命令 */
bool voice_esp_sr_is_awake(void);

/* 功能：给屏幕留一句话 */
typedef struct {
    volatile int pending;   /* 功能：0 没新的 1 有新的 */
    char         text[48];  /* 功能：提示文字 */
} voice_ui_note_t;

extern voice_ui_note_t g_voice_ui_note;

/* 功能：写一条屏幕提示 */
void voice_ui_note(const char *text);

#ifdef __cplusplus
}
#endif
