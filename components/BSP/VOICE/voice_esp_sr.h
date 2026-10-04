/**
 * @file  voice_esp_sr.h
 * @brief 语音识别来源之二：INMP441 I²S 麦克风 + 乐鑫 ESP-SR 离线识别
 *
 * ===========================================================================
 *  这一层在整个工程里的位置（为什么上层一行都不用改）
 * ===========================================================================
 *  voice.c 是"语音抽象层"，它把「说一句话」抽象成 voice_cmd_t + 一个回调，
 *  App 层（main.c::voice_on_cmd / MQTT / OLED / 自动联动）只认 voice_cmd_t。
 *
 *  本文件负责的是【换掉抽象层下面的实现】：
 *
 *      ASRPRO 模块（UART1，单字节/ASCII 文本）
 *                    ↘
 *                      voice_dispatch() → App 层回调     ← 两条路汇到同一个点
 *                    ↗
 *      INMP441 + ESP-SR（本文件：唤醒词 + 命令词 → voice_cmd_t）
 *
 *  所以本文件【不碰】任何业务逻辑：识别到什么就翻译成 voice_cmd_t 再调
 *  voice_dispatch()，和 ASRPRO 那条路完全一样。
 *
 * ===========================================================================
 *  编译期开关（menuconfig）
 * ===========================================================================
 *  Component config → 智能家居 → 应用行为 → 语音识别来源使用 ESP-SR
 *      CONFIG_APP_VOICE_SOURCE_ESP_SR
 *  · 关（默认）：本文件的代码全部被 #if 编译掉，工程回到 ASRPRO UART 方案，
 *                行为与本任务之前【完全一致】；
 *  · 开        ：voice_init() 会调用 voice_esp_sr_start()，
 *                由本文件接管拾音 + 识别 + 翻译。
 *
 * ===========================================================================
 *  ⚠ 没有喇叭 = 没有语音播报（要说清楚，免得当成 bug）
 * ===========================================================================
 *  ASRPRO 模块自带喇叭接口，所以老方案里 voice_speak() 能让模块"说话"。
 *  INMP441 只有【麦克风】，没有功放/喇叭，所以 ESP-SR 方案下：
 *      · 设备控制类指令（开灯/关灯/开风扇…）：完全正常，灯会真的亮；
 *      · 查询类指令（现在温度/播报全部…）：会正常走完业务链路
 *        （OLED 显示、MQTT 上报事件都会发生），但【不会出声】。
 *  esp-sr 自带中文 TTS（esp-tts/esp_tts_chinese），将来要做语音播报，
 *  需要另加一路 I²S 功放（如 MAX98357A）+ 喇叭，本任务不涉及。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 ESP-SR 离线语音识别（幂等）
 *
 * 做的事（每一步失败都只告警并回退，绝不崩，符合本工程一贯风格）：
 *   1. i2s_mic_init()          拉起 INMP441（16KHz/16bit/单声道）
 *   2. esp_srmodel_init("model") 从 model 分区 mmap 模型（不看文件系统）
 *   3. 选出唤醒词模型（wn*）与命令词模型（mn*），打印【实际选中的唤醒词】
 *   4. afe_config_init("M", ...) + esp_afe_handle_from_config() + create_from_config()
 *   5. multinet->create() + 用运行时 API 注册 25 条中文命令词（拼音格式）
 *   6. 起识别任务：feed 音频 → 唤醒 → 命令词 → 翻译成 voice_cmd_t → dispatch
 *
 * @return ESP_OK 已启动（或已经在跑）；其它 = 启动失败，调用方只告警即可
 */
esp_err_t voice_esp_sr_start(void);

/** @brief 识别引擎是否已经跑起来（初始化全部成功） */
bool voice_esp_sr_is_ready(void);

/**
 * @brief 当前唤醒词的显示名（UTF-8，例如"你好小智"）
 *
 * 从模型自带的 _MODEL_INFO_ 里解析出来的，所以它一定和实际烧进 flash 的
 * 模型一致 —— 用户必须知道要喊哪个词，这个函数就是那个答案的来源。
 */
const char *voice_esp_sr_wake_word(void);

/** @brief 当前命令词模型的显示名（例如 "mn7_cn"） */
const char *voice_esp_sr_mn_model(void);

/**
 * @brief 打印完整命令词表（串口调试台 `voice-test` 用）
 *
 * 打印内容：唤醒词、序号、中文命令词、交给 MultiNet 的拼音、对应的 voice_cmd_t。
 * 用户照着这张表念就能测，不需要去看源码。
 */
void voice_esp_sr_print_commands(void);

/** @brief 识别任务是否处于"已唤醒、正在等命令词"状态（OLED/调试用，可不用） */
bool voice_esp_sr_is_awake(void);

/**
 * @brief 语音交互的 OLED 提示（本方案没有喇叭，唤醒/识别成功的"反馈"
 *        用屏幕弹窗替代，用户不用盯着串口）
 *
 * 语音层（识别任务里）只负责【写文字】，显示由 astra UI 任务轮询消费：
 * 不能直接在识别任务里调弹窗 —— 弹窗动画是阻塞循环，会拖垮实时识别。
 * 文字示例："已唤醒，请说命令" / "已识别:打开厨房灯"。
 */
typedef struct {
    volatile int pending;   /* 0=无新提示 1=有新提示（消费方读后清零） */
    char         text[48];  /* UTF-8 提示文字（先写 text 再置 pending） */
} voice_ui_note_t;

extern voice_ui_note_t g_voice_ui_note;

/** @brief 写一条 UI 提示（覆盖上一条尚未消费的） */
void voice_ui_note(const char *text);

#ifdef __cplusplus
}
#endif
