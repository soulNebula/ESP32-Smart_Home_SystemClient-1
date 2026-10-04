/**
 * @file  voice_esp_sr.c
 * @brief INMP441 I²S 麦克风 + 乐鑫 ESP-SR 离线语音识别（唤醒词 + 命令词）
 *
 * ===========================================================================
 *  整体数据流
 * ===========================================================================
 *
 *   INMP441 ──I²S(16K/16bit/mono)──> i2s_mic_read()
 *                                        │
 *                                        ▼
 *                              AFE feed()（噪声抑制 + 增益 + VAD）
 *                                        │
 *                                        ▼
 *                              AFE fetch()（单通道增强后的 16K16bit 音频）
 *                                        │
 *                        ┌───────────────┴────────────────┐
 *                        ▼                                ▼
 *              wakeup_state == DETECTED            命令词阶段
 *              （唤醒词命中了）                      multinet->detect()
 *                        │                                │
 *                        └────────► 识别结果 ─────────────┘
 *                                        │
 *                                        ▼
 *                     查表翻译成 voice_cmd_t → voice_dispatch()
 *                                        │
 *                                        ▼
 *                     App 层 main.c::voice_on_cmd()（一行都没改）
 *
 * ===========================================================================
 *  【设计要点 / 为什么这么写】
 * ===========================================================================
 *  1) 命令词用【运行时 API】注册，不用 menuconfig 里的命令词列表。
 *     实证（读 esp-sr 源码 esp_process_sdkconfig.c）：
 *        esp_mn_commands_update_from_sdkconfig() 一开头就是
 *        `#if defined CONFIG_SR_MN_CN_MULTINET6_QUANT || ... MULTINET7 ...
 *            return NULL;`
 *     —— MultiNet6/7 直接返回 NULL，Kconfig 里那 165 条命令词选项对它们【无效】。
 *     所以 MultiNet7 必须走 esp_mn_commands_alloc/add/update 这条运行时路径。
 *     好处：command_id 由我们自己指定，一条命令一个 voice_cmd_t，映射干净。
 *
 *  2) 命令词写【拼音】而不是汉字。
 *     实证（同一次源码阅读）：
 *        esp_mn_commands_add() 里 `check_speech_command(model_data, string)` 的入参
 *        是拼音，且 vocab 文件里的 token 形如 "▁da" "▁kai"（乐鑫官方
 *        model/multinet_model/fst/commands_cn.txt 也是 "1,da kai kong tiao"）。
 *     MultiNet7 中文的识别单元是【带词界的拼音音节】，所以这里写拼音。
 *     文档里同时给出中文和拼音两列，用户照着中文念就行。
 *
 *  3) 识别任务用【低优先级 + 固定栈】跑，并主动 vTaskDelay(1) 让出 CPU：
 *     ESP-SR 的 AFE 内部给 feed/fetch 各有一个环形缓冲，只要平均消费速度
 *     跟得上，偶尔被 WiFi/BLE 抢占也不会丢数据。反而是"一直占着 CPU 不放"
 *     会饿死 WiFi 任务、影响 MQTT 心跳 —— 那才是真正的共存风险。
 *
 *  4) 顺序上【先起网络（WiFi/MQTT/BLE），再起语音】：语音是最"重"的一块，
 *     放最后可以让开机日志里先出现 WiFi/MQTT 成功，出事时一眼能看出是谁的问题；
 *     而且 ESP-SR 初始化会吃掉相当多的内存/PSRAM，晚一点分配能避开和网络
 *     初始化抢堆的峰值。main.c 里的调用顺序已经保证了这一点。
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "i2s_mic.h"
#include "voice.h"
#include "voice_esp_sr.h"
#include "voice_internal.h"     /* voice_dispatch() —— 与 ASRPRO 方案共用的唯一汇合点 */

#if CONFIG_APP_VOICE_SOURCE_ESP_SR

/* ---- ESP-SR 头文件（只在开关打开时才包含，保证关掉开关时不依赖该组件） ---- */
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"   /* check_chip_config() */
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

/* ===========================================================================
 *  编译期自检：唤醒词 / 命令词模型必须【至少各选一个】
 * ===========================================================================
 *  模型选项（CONFIG_SR_WN_* / CONFIG_SR_MN_*）住在 esp-sr 组件自己的
 *  Kconfig 菜单里（菜单路径见 main/Kconfig.projbuild 的注释），
 *  这里用 #if defined() 做一致性检查。
 *
 *  为什么不在 Kconfig 里强制：那些选项带 prompt（用户可以改），既不能被
 *  select，在别的 Kconfig 文件里重复定义同名 config 又有产生重名符号的风险。
 *  所以退而求其次：把检查放到编译期 —— 一个都没选就【编译不过】，并直接把
 *  "该去哪个菜单选什么"打在编译错误里，比烧录后运行时才发现好得多。
 *
 *  注意 esp-sr 的 Kconfig 逐个列了唤醒词（wn9s_* / wn9_* / wn7_* …），
 *  这里不可能穷举所有型号，所以只在"一个都没有"时报错；
 *  具体加载哪个由 voice_sr_pick_models() 从 model 分区里自己挑（见该函数）。
 *
 *  ⚠ 本段注释里描述的那个坑（2026-09 实测踩到两次，记录一下免得后人重复）：
 *     下面的 #if 是多行的，行尾用反斜杠做续行。而 C 的【块注释】里出现
 *     "反斜杠 + 换行"时会触发编译错误，报的是
 *         error: 引号内的注释起始符 within comment [-Werror=comment]
 *     构建直接失败。教训有两条：
 *       · 不要在块注释里让某一行以反斜杠结尾；
 *       · 不要在块注释里写出注释的起始符号本身（哪怕是用引号包着）。
 *     本文件这条注释就因为这个坑返工过，写在这里当路标。
 * =========================================================================== */
// clang-format off
#if !defined(CONFIG_SR_WN_WN9S_HILEXIN) && !defined(CONFIG_SR_WN_WN9S_HIESP) && \
    !defined(CONFIG_SR_WN_WN9S_NIHAOXIAOZHI) && !defined(CONFIG_SR_WN_WN9S_HIJASON) && \
    !defined(CONFIG_SR_WN_WN9_HILEXIN) && !defined(CONFIG_SR_WN_WN9_HIESP) && \
    !defined(CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS) && !defined(CONFIG_SR_WN_WN9L_NIHAOXIAOZHI_TTS3) && \
    !defined(CONFIG_SR_WN_WN9_XIAOAITONGXUE) && !defined(CONFIG_SR_WN_WN9L_XIAOAITONGXUE) && \
    !defined(CONFIG_SR_WN_WN9_NIHAOMIAOBAN_TTS2) && !defined(CONFIG_SR_WN_WN8_HILEXIN) && \
    !defined(CONFIG_SR_WN_WN8_HIESP) && !defined(CONFIG_SR_WN_WN8_ALEXA) && \
    !defined(CONFIG_SR_WN_WN9_ALEXA)
#error "已打开 CONFIG_APP_VOICE_SOURCE_ESP_SR，但没有选择任何【唤醒词模型】。请运行 idf.py menuconfig → 'ESP Speech Recognition' → 'Load Multiple Wake Words (WakeNet9s)'，勾选一个（推荐 你好小智 / wn9s_nihaoxiaozhi）。"
#endif

#if !defined(CONFIG_SR_MN_CN_MULTINET5_RECOGNITION_QUANT8) && \
    !defined(CONFIG_SR_MN_CN_MULTINET6_QUANT) && \
    !defined(CONFIG_SR_MN_CN_MULTINET7_QUANT) && \
    !defined(CONFIG_SR_MN_CN_MULTINET7_AC_QUANT) && \
    !defined(CONFIG_SR_MN_CN_MULTINET6_AC_QUANT)
#error "已打开 CONFIG_APP_VOICE_SOURCE_ESP_SR，但没有选择任何【中文命令词模型】。请运行 idf.py menuconfig → 'ESP Speech Recognition' → 'Chinese Speech Commands Model'，勾选一个（推荐 general chinese recognition / mn7_cn）。"
#endif
// clang-format on


static const char *TAG = "VOICE_SR";

/* -------------------------------------------------------------------------- */
/*  参数                                                                       */
/* -------------------------------------------------------------------------- */
#define VOICE_SR_TASK_STACK     12288   /* 识别任务栈（字节）。
                                         * ★ 给这么大有具体理由，不是保险起见：
                                         *   本任务里会【直接】调 voice_dispatch()
                                         *   → 用户回调 main.c::voice_on_cmd()
                                         *   → device_model → mqtt_publish_state
                                         *     （cJSON 递归 + 事件发布）。
                                         *   voice.c 的文件头记录过：那条链在 3072 字节
                                         *   栈上【实测栈溢出 panic】（语音命令后必崩）。
                                         *   这里再叠加 ESP-SR 自身的调用深度，
                                         *   所以给 12KB（内部 RAM 够，见启动日志）。 */
#define VOICE_SR_TASK_PRIO      7       /* ★ 2026-10-02 实测：唤醒后 MultiNet detect 每条
                                         * 帧耗时数百毫秒，识别任务(3)被同核的喂帧任务(5)
                                         * 挤到只剩 ~10% CPU → AFE 喂帧缓冲溢出、命令词
                                         * 无法识别（6 秒窗口 52 秒才走完）。抬到 7（高于
                                         * 喂帧 5）：识别时它拿满 CPU，喂帧有 60ms I²S DMA
                                         * + 500ms AFE 环形缓冲兜底，短暂让路不会丢音频。 */
#define VOICE_SR_TASK_CORE      1       /* 绑到核心 1：核心 0 主要跑 WiFi/BLE 协议栈，
                                         * 减少缓存争抢（本工程 PS 任务未绑核，这里主动避让） */

#define VOICE_SR_MN_DURATION_MS 6000    /* 唤醒后命令词等待窗口：6 秒内没说出命令就回休眠。
                                         * 太短：老人/孩子一句话没说完就超时；
                                         * 太长：一直处于"听命令"状态，更容易被电视声误触发。 */
#define VOICE_SR_READ_TIMEOUT_MS 1000   /* 单次读音频的超时（毫秒） */

/* ---- 喂帧任务（独立任务，理由见 voice_sr_feed_task 上方的长注释）----
 * 优先级【高于】识别任务：喂帧是实时性要求最高的环节，一旦被拖慢，
 * AFE 就会永久欠载并刷屏。它绝大部分时间阻塞在 I²S 读上，并不占 CPU。 */
#define VOICE_SR_FEED_STACK      12288  /* 喂帧任务栈：只做「读 + feed()」。
                                         * ★ 实测教训：最初给 4096，真机跑起来
                                         *   立刻 panic "A stack overflow in task
                                         *   voice_feed"。原因是 afe->feed() 内部
                                         *   会跑 NS/VAD/WakeNet 的 DSP 流水线，
                                         *   调用深度比"只读个 I²S"大得多。
                                         *   这里和识别任务同量级，并且状态日志里
                                         *   打印栈余量（HWM）以便日后按实测收窄。 */
#define VOICE_SR_FEED_PRIO       5      /* 高于识别任务(3)、高于按键扫描(4)。
                                         * ★ 曾一度提到 15，理由是"喂帧只有 62/秒、
                                         *   69% 时间没被调度"—— 那是误读（62 是
                                         *   fetch 频率）。加探针实测占用 99%、
                                         *   喂帧 199 帧/秒，**不存在调度饥饿**，
                                         *   所以改回 5：没有实测依据就不要把实时
                                         *   任务抬到所有应用任务之上。 */

/* 识别任务 fetch 的等待超时（毫秒）。
 * ★ 必须是个真实值：0 = 空轮询，AFE 会被反复"问空"并疯狂刷屏
 *   （真机实测 155 行/秒，占满日志，且靠串口写阻塞反过来拖慢喂帧）。
 *   100ms 远大于一帧(10ms)，正常情况下 fetch 立刻返回；即使喂帧真出问题，
 *   刷屏速率也被限制在 ~10 条/秒，不会淹没串口调试台。 */
#define VOICE_SR_FETCH_TIMEOUT_MS 100

/* -------------------------------------------------------------------------- */
/*  命令词表 —— ★★ 这是"说中文 → voice_cmd_t"的唯一映射真源 ★★               */
/* -------------------------------------------------------------------------- */
/*  三列含义：
 *      cn     ：中文命令词，给用户看的（文档和 `voice-test` 打印的就是它）
 *      pinyin ：交给 MultiNet 的实际字符串，必须是"带空格的拼音音节"
 *               （MultiNet6/7 的识别单元，见文件头设计要点 2）
 *      cmd    ：翻译成的 voice_cmd_t，下游 App 层只认这个
 *
 *  ⚠ 改动这里必须同步改 docs/13-语音模块-ESP-SR.md 里的命令词表。
 *  ⚠ 命令词之间要有明显差异：MultiNet 是按音节匹配的，
 *    "打开窗户(da kai chuang hu)" 和 "打开窗帘(da kai chuang lian)" 前三个音节
 *    完全相同，容易互相抢；所以窗帘用"拉开/拉上"，和窗户分开。
 *  ⚠ 命令词总数（25 条）远小于模型上限（200 条）；条数越多越容易误识别，
 *    以后加词请优先加差异大的。
 */
typedef struct {
    const char *cn;         /* 中文命令词（UTF-8） */
    const char *pinyin;     /* 拼音音节串（MultiNet 的识别单元） */
    voice_cmd_t cmd;        /* 翻译成的指令 */
} voice_sr_cmd_t;

static const voice_sr_cmd_t s_cmds[] = {
    /* ---- 单个房间灯（4 个房间 × 开关） ---- */
    { "打开客厅灯",   "da kai ke ting deng",     VOICE_CMD_LED_LIVING_ON    },
    { "关闭客厅灯",   "guan bi ke ting deng",    VOICE_CMD_LED_LIVING_OFF   },
    { "打开厨房灯",   "da kai chu fang deng",    VOICE_CMD_LED_KITCHEN_ON   },
    { "关闭厨房灯",   "guan bi chu fang deng",   VOICE_CMD_LED_KITCHEN_OFF  },
    { "打开卧室灯",   "da kai wo shi deng",      VOICE_CMD_LED_BEDROOM_ON   },
    { "关闭卧室灯",   "guan bi wo shi deng",     VOICE_CMD_LED_BEDROOM_OFF  },
    { "打开浴室灯",   "da kai yu shi deng",      VOICE_CMD_LED_BATH_ON      },
    { "关闭浴室灯",   "guan bi yu shi deng",     VOICE_CMD_LED_BATH_OFF     },

    /* ---- 全部灯 ---- */
    { "打开全部灯",   "da kai quan bu deng",     VOICE_CMD_LED_ALL_ON       },
    { "关闭全部灯",   "guan bi quan bu deng",    VOICE_CMD_LED_ALL_OFF      },

    /* ---- 风扇 ---- */
    { "打开风扇",     "da kai feng shan",        VOICE_CMD_FAN_ON           },
    { "关闭风扇",     "guan bi feng shan",       VOICE_CMD_FAN_OFF          },

    /* ---- 窗户（用"打开/关闭窗户"，不与窗帘混） ---- */
    { "打开窗户",     "da kai chuang hu",        VOICE_CMD_WINDOW_OPEN      },
    { "关闭窗户",     "guan bi chuang hu",       VOICE_CMD_WINDOW_CLOSE     },

    /* ---- 门 ---- */
    { "打开门",       "da kai men",              VOICE_CMD_DOOR_OPEN        },
    { "关上门",       "guan shang men",          VOICE_CMD_DOOR_CLOSE       },

    /* ---- 窗帘（用"拉开/拉上"，与窗户彻底分开） ---- */
    { "拉开窗帘",     "la kai chuang lian",      VOICE_CMD_CURTAIN_OPEN     },
    { "拉上窗帘",     "la shang chuang lian",    VOICE_CMD_CURTAIN_CLOSE    },

    /* ---- 查询播报（⚠ 没有喇叭，不会出声；但 OLED/MQTT 事件照常） ---- */
    { "温度多少",     "wen du duo shao",         VOICE_CMD_QUERY_TEMP       },
    { "湿度多少",     "shi du duo shao",         VOICE_CMD_QUERY_HUMI       },
    { "光照多少",     "guang zhao duo shao",     VOICE_CMD_QUERY_LIGHT      },
    { "播报全部",     "bao bao quan bu",         VOICE_CMD_QUERY_ALL        },
    { "状态如何",     "zhuang tai ru he",        VOICE_CMD_QUERY_STATUS     },

    /* ---- 自动联动总开关 ---- */
    { "打开自动",     "da kai zi dong",          VOICE_CMD_AUTO_ON          },
    { "关闭自动",     "guan bi zi dong",         VOICE_CMD_AUTO_OFF         },
};

#define VOICE_SR_CMD_NUM (sizeof(s_cmds) / sizeof(s_cmds[0]))

/* -------------------------------------------------------------------------- */
/*  内部状态                                                                   */
/* -------------------------------------------------------------------------- */
static bool              s_started   = false;   /* start 幂等标志 */
static volatile bool     s_ready     = false;   /* 初始化全部成功？ */
static volatile bool     s_awake     = false;   /* 已唤醒、正在等命令词？ */

static srmodel_list_t   *s_models       = NULL; /* model 分区里的模型列表 */
static char             *s_wn_name      = NULL; /* 选中的唤醒词模型名，如 wn9s_nihaoxiaozhi */
static char             *s_mn_name      = NULL; /* 选中的命令词模型名，如 mn7_cn */
static char              s_wake_word[64] = { 0 }; /* 唤醒词显示名（UTF-8） */

static const esp_afe_sr_iface_t *s_afe      = NULL;
static esp_afe_sr_data_t        *s_afe_data = NULL;
static int                       s_feed_chunksize = 0;

static const esp_mn_iface_t *s_mn      = NULL;
static model_iface_data_t   *s_mn_data = NULL;

static int16_t *s_feed_buf = NULL;      /* AFE feed 缓冲（PSRAM） */

/* -------------------------------------------------------------------------- */
/*  模型选择与命令词注册                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 打印 model 分区里实际有哪些模型，并挑选唤醒词/命令词模型
 *
 * 为什么要"自己挑"而不是写死名字：模型是 movemodel.py 根据 sdkconfig 打包进
 * srmodels.bin 的，换 menuconfig 选项后实际名字会变；自己挑一次就能保证
 * 代码永远和烧进去的模型一致，也能在日志里把"实际选中的"打给用户看。
 */
static esp_err_t voice_sr_pick_models(void)
{
    if (s_models == NULL) {
        ESP_LOGE(TAG, "模型列表为空，无法挑选模型");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1) 把分区里所有模型名列出来 —— 排错时这一行日志最有用：
     *    如果这里没有以 wn 或 mn 开头的模型，说明 srmodels.bin 没烧进
     *    model 分区，或者 menuconfig 里模型选项一个都没勾。
     *    ★ 注意：这里刻意不写出"星号紧跟斜杠"那种通配写法 ——
     *      在块注释里那个组合就是注释结束符，会把后面的中文当代码编译。 */
    ESP_LOGI(TAG, "model 分区里共有 %d 个模型：", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, s_models->model_name[i]);
    }

    /* 2) 唤醒词模型：名字以 "wn" 开头（esp-sr 自己的 ESP_WN_PREFIX 就是 "wn"）。
     *
     * 为什么不直接 esp_srmodel_filter(models, "wn", NULL) 取第一个：
     *   如果哪天用户在 menuconfig 里勾了【多个】唤醒词，返回顺序是不确定的，
     *   日志里就会看到"选中的唤醒词"飘忽不定。这里自己遍历一遍，按
     *   wn9s_ → wn9l_ → wn9_ → wn8_ → wn7_ 的优先顺序挑，并且把【所有】
     *   唤醒词都列出来 —— 用户勾了多个时，日志能直接看出来。
     *   （Kconfig 的 help 里已经写明"必须恰好选一个"，这里只是兜底。） */
    {
        static const char *const prefer[] = { "wn9s_", "wn9l_", "wn9_", "wn8_", "wn7_", NULL };
        for (int p = 0; prefer[p] != NULL && s_wn_name == NULL; p++) {
            for (int i = 0; i < s_models->num; i++) {
                if (strstr(s_models->model_name[i], prefer[p]) != NULL) {
                    s_wn_name = s_models->model_name[i];
                    break;
                }
            }
        }
    }
    if (s_wn_name == NULL) {
        ESP_LOGE(TAG, "model 分区里找不到唤醒词模型（wn*）—— "
                      "检查 menuconfig → ESP Speech Recognition → "
                      "Load Multiple Wake Words (WakeNet9s) 是否勾了模型");
        return ESP_ERR_NOT_FOUND;
    }

    /* 3) 中文命令词模型：先按 "mn*cn" 找（例如 mn7_cn），
     *    找不到再放宽到 "mn"（万一以后模型名规则变了也不至于直接失败）。 */
    s_mn_name = esp_srmodel_filter(s_models, "mn", ESP_MN_CHINESE);
    if (s_mn_name == NULL) {
        s_mn_name = esp_srmodel_filter(s_models, "mn", NULL);
    }
    if (s_mn_name == NULL) {
        ESP_LOGE(TAG, "model 分区里找不到命令词模型（mn*）—— "
                      "检查 menuconfig → ESP Speech Recognition → Chinese Speech Commands Model");
        return ESP_ERR_NOT_FOUND;
    }

    /* 4) 解析唤醒词的"人话"名字（要喊什么），用于日志和文档。
     *    模型自带的 _MODEL_INFO_ 形如：
     *        wakenet9s_tts2h8v2_你好小智_3_0.630_0.635
     *    → esp_srmodel_get_wake_words() 会解析出 "你好小智"。
     *    这样文档里写的唤醒词一定和实际烧录的模型一致。 */
    char *ww = esp_srmodel_get_wake_words(s_models, s_wn_name);
    if (ww != NULL) {
        strncpy(s_wake_word, ww, sizeof(s_wake_word) - 1);
        s_wake_word[sizeof(s_wake_word) - 1] = '\0';
        free(ww);
    } else {
        strncpy(s_wake_word, "(未知，看模型名)", sizeof(s_wake_word) - 1);
    }

    ESP_LOGI(TAG, "★ 唤醒词模型 = %s   要喊的词 = 「%s」", s_wn_name, s_wake_word);
    ESP_LOGI(TAG, "★ 命令词模型 = %s", s_mn_name);
    return ESP_OK;
}

/**
 * @brief 用运行时 API 把 s_cmds[] 注册进 MultiNet
 *
 * command_id 直接用 voice_cmd_t 的枚举值：这样识别结果里的 command_id
 * 本身就是 voice_cmd_t，不用再维护第二张映射表 —— 少一处会写错的地方。
 * （命令词枚举值是 1~25，而模型要求 command_id 不能为 0，正好满足。）
 */
static esp_err_t voice_sr_register_commands(void)
{
    if (s_mn == NULL || s_mn_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_mn_commands_alloc(s_mn, s_mn_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mn_commands_alloc failed: %s", esp_err_to_name(err));
        return err;
    }

    int ok = 0, bad = 0;
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        /* 注意：命令词用【拼音】。MultiNet 的 check_speech_command() 会对
         * 每个词做一次词表校验，非法格式会在这里返回错误并打自己的日志。 */
        const esp_err_t e = esp_mn_commands_add((int)s_cmds[i].cmd, s_cmds[i].pinyin);
        if (e == ESP_OK) {
            ok++;
        } else {
            bad++;
            ESP_LOGW(TAG, "命令词注册失败：%s (%s) -> %s",
                     s_cmds[i].cn, s_cmds[i].pinyin, esp_err_to_name(e));
        }
    }

    /* 必须调 update 才会真正把命令词编译进语言模型 */
    esp_mn_error_t *mn_err = esp_mn_commands_update();
    if (mn_err != NULL && mn_err->num > 0) {
        ESP_LOGW(TAG, "有 %d 条命令词无法被 MultiNet 解析（识别时不会命中）：",
                 (int)mn_err->num);
        for (int i = 0; i < mn_err->num; i++) {
            ESP_LOGW(TAG, "  %s", mn_err->phrases[i]->string);
        }
    }

    ESP_LOGI(TAG, "命令词注册完成：成功 %d / 失败 %d，共 %d 条",
             ok, bad, (int)VOICE_SR_CMD_NUM);
    return (ok > 0) ? ESP_OK : ESP_FAIL;
}

/* -------------------------------------------------------------------------- */
/*  运行统计（只给日志用）                                                     */
/* -------------------------------------------------------------------------- */
/*  ★ 这几个数字是本次缺陷排查的关键。上一版把"喂帧数"打在 fetch 之后，
 *    而 fetch 一失败就 `continue`，结果【一次都没打出来】。              */
static volatile uint32_t s_st_feed_ok   = 0;    /* 成功喂进 AFE 的帧数 */
static volatile uint32_t s_st_feed_fail = 0;    /* 读不满一帧 / feed() 失败 次数 */
static volatile uint32_t s_st_zero      = 0;    /* 全 0 帧数（麦克风没接的典型表现） */

/* 喂帧任务的句柄：只为了在状态日志里查它的栈余量（HWM）。
 * 真机踩过 "stack overflow in task voice_feed"，所以要把余量暴露出来。 */
static TaskHandle_t      s_feed_task_h  = NULL;

/* 喂帧耗时探针（窗口内累计，状态日志打印后清零）。
 * ★ 为什么需要它，以及它纠正了什么错误结论（真实排查记录）：
 *   最初把状态日志里的 w_frames（fetch 成功次数 = 62 次/秒）误当成"喂帧速率"，
 *   于是判断"喂帧只有 62/秒 < 目标 100，丢了 38% 音频"。加了本探针之后实测：
 *       读 997 次 / 5 秒 = 199 次/秒，feed 997 次 / 5 秒 = 199 帧/秒，
 *       占用 99%，读均 4.7ms、feed 均 0.3ms。
 *   即【喂帧一直是达标的，199 > 100】；62 只是 fetch 频率，而
 *       fetch_chunksize(512) / feed_chunksize(160) = 3.2，199/3.2 = 62 ✔
 *   两个量本来就不同，拿来对比才凭空造出一个不存在的问题。
 *   这也是为什么探针里【同时】记录调用次数与耗时：只有均值没有次数，
 *   连"占用率"都算不出来（当时就因此误判成"任务被抢占、69% 时间没跑"）。
 *   注：占用率把 i2s_mic_read() 的阻塞等待也算进去，所以它只说明
 *       "时间花在哪段代码上"，不等于 CPU 忙。 */
static volatile uint32_t s_t_read_us = 0;   /* 窗口内 i2s_mic_read() 累计微秒 */
static volatile uint32_t s_t_reads   = 0;   /* 窗口内 read 调用次数 */
static volatile uint32_t s_t_feed_us = 0;   /* 窗口内 afe->feed() 累计微秒 */
static volatile uint32_t s_t_feeds   = 0;   /* 窗口内 feed 调用次数 */

/* ---- 诊断探针（唤醒问题排查；确认修复后可整体删除）----
 * 唤醒词喊了没反应时，光靠电平表只能证明"麦克风在工作"，看不到
 * AFE 内部的 VAD 门和 wakenet 状态。这里补两个只打【变化/限速】的日志：
 *   · VAD 状态翻转（静音↔说话）—— 证明声音有没有被 VAD 判成语音；
 *   · wakenet 输入电平（res->data_volume，wakenet 接收窗约 1.5s 的 dB 值）
 *     —— 说话时它会明显抬高，和串口 `mic` 的电平互相印证。 */
static volatile int32_t s_diag_last_vad     = -1;   /* 上次的 VAD 状态（初值 -1 保证首帧必打） */
static int64_t          s_diag_last_vol_us  = 0;    /* wakenet 输入电平日志的限速 */
static volatile uint32_t s_diag_detect_us   = 0;    /* 窗口内 MultiNet detect() 累计微秒 */
static volatile uint32_t s_diag_detect_n    = 0;    /* 窗口内 detect() 调用次数 */

/* ---- OLED 提示（见 voice_esp_sr.h 的说明：本方案无喇叭，反馈靠屏幕） ---- */
voice_ui_note_t g_voice_ui_note = { 0, { 0 } };

void voice_ui_note(const char *text)
{
    /* 先写文字、再置 pending：消费方看到 pending=1 时文字一定完整 */
    snprintf(g_voice_ui_note.text, sizeof(g_voice_ui_note.text), "%s", text);
    g_voice_ui_note.pending = 1;
}

/** @brief 按 voice_cmd_t 反查中文命令词（OLED 提示用） */
static const char *voice_sr_cmd_cn(voice_cmd_t cmd)
{
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        if (s_cmds[i].cmd == cmd) {
            return s_cmds[i].cn;
        }
    }
    return "未知指令";
}

/* -------------------------------------------------------------------------- */
/*  喂帧任务：只做「读 I²S → AFE feed()」这一件事                              */
/* -------------------------------------------------------------------------- */
/*  ★ 为什么必须【独立成一个任务】，而不是和识别挤在同一个 for(;;) 里 ★
 *
 *  这是真机实测逼出来的，不是"架构洁癖"。原先的写法实测：
 *      AFE 底层 "Ringbuffer of AFE is empty, Please use feed() to write data"
 *      稳定刷屏 155 行/秒，把日志占掉 93%，串口调试台完全没法用。
 *
 *  ★ 根因（实测确认后的说法，早先的推测是错的）：
 *      `fetch_with_delay(afe_data, 0)` —— 超时给 0 = 空轮询。
 *      环形缓冲还没攒够一帧时它照样去取，取不到 AFE 就打一条告警；
 *      而那条告警走 115200 串口是【写阻塞】的，反过来拖慢循环，
 *      于是"越问越空、越空越打"。
 *      ★ 关键：当时【喂帧速率本来是达标的】（后来加了探针实测 199 帧/秒
 *        > 目标 100），所以这条刷屏与"喂得慢"无关，纯粹是【把非阻塞
 *        fetch 当轮询用】。早先归因为"喂帧只有 55/秒导致 AFE 欠载"是误读
 *        —— 55 其实是 fetch 成功次数（fetch_chunksize 512 = 3.2×feed 160）。
 *
 *  那为什么还要拆成两个任务？因为**阻塞式 fetch 必须有个专门的喂帧者**：
 *      识别任务一旦阻塞在 fetch 里等数据，就没人读 I²S、没人 feed 了 ——
 *      这正是原设计只能退化成空轮询的原因（同一个循环里 feed 和 fetch
 *      不可能同时阻塞）。
 *
 *  拆开之后：
 *      · 喂帧任务只认 I²S 的节奏，持续按实时速率喂，不受识别/串口/无线影响；
 *      · 识别任务用【阻塞式】fetch 安静地等数据，等不到只返回 NULL，
 *        不再去"问空" AFE —— 那条告警于是自然消失（实测 4752 行 → 2 行，
 *        且剩下 2 行都出现在喂帧任务启动之前的启动窗口内）。
 *
 *  这也是乐鑫官方 ESP-SR 示例的结构：feed 一个任务、detect 一个任务。   */
static void voice_sr_feed_task(void *arg)
{
    (void)arg;

    /* 启动阶段：等 I²S/麦克风稳定。INMP441 上电后前 ~50ms 数据无效，
     * 另外 AFE 自己也需要几帧才能把降噪/AGC 收敛。 */
    vTaskDelay(pdMS_TO_TICKS(300));

    int64_t last_zero_warn_us = 0;

    ESP_LOGI(TAG, "喂帧任务启动：每帧 %d 采样点（%d ms @%dHz），目标 %d 帧/秒",
             s_feed_chunksize,
             (s_feed_chunksize * 1000) / BSP_I2S_MIC_SAMPLE_RATE,
             BSP_I2S_MIC_SAMPLE_RATE,
             BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize);

    for (;;) {
        /* ---- 读满【正好】一帧 ----
         * i2s_mic_read() 内部会一直读到凑满 samples 或超时预算耗尽，
         * 所以正常情况下一次调用就拿到一整帧（160 点 = 10ms @16KHz）。 */
        const int64_t t_read0 = esp_timer_get_time();
        size_t got = 0;
        const esp_err_t err = i2s_mic_read(s_feed_buf, (size_t)s_feed_chunksize,
                                           &got, VOICE_SR_READ_TIMEOUT_MS);
        s_t_read_us += (uint32_t)(esp_timer_get_time() - t_read0);
        s_t_reads++;

        if (got < (size_t)s_feed_chunksize) {
            /* 没凑满一帧：AFE 只接受完整帧，这一轮不喂。
             * ★ 只在【真没数据】时让出 CPU；读满了立刻读下一帧，不加 sleep，
             *   否则会白白压低喂帧速率。 */
            s_st_feed_fail++;
            if ((s_st_feed_fail == 1) || ((s_st_feed_fail % 100) == 0)) {
                ESP_LOGW(TAG, "读麦克风未凑满一帧 %u 次（%s，本次 %u/%d 点）："
                              "麦克风可能没接，语音识别暂不可用"
                              "（系统其它功能不受影响）",
                         (unsigned)s_st_feed_fail, esp_err_to_name(err),
                         (unsigned)got, s_feed_chunksize);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ---- 全 0 检测 ----
         * 麦克风没接时 I²S 数据脚被拉低，读回来全是 0。这种帧照样喂给 AFE
         * （保持时序、不让它欠载），只是周期性地提醒用户一次。 */
        bool all_zero = true;
        for (int i = 0; i < s_feed_chunksize; i++) {
            if (s_feed_buf[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) {
            s_st_zero++;
            const int64_t now = esp_timer_get_time();
            if (now - last_zero_warn_us > 30LL * 1000 * 1000) {
                last_zero_warn_us = now;
                ESP_LOGW(TAG, "连续收到全 0 音频帧 %u 个（I²S SD 脚可能是低电平）："
                              "麦克风没接？串口敲 `mic` 看实时电平。"
                              "此告警每 30 秒最多一条。", (unsigned)s_st_zero);
            }
        }

        /* ---- 喂 AFE ----
         * feed() 返回"消费掉的采样点数"，负数表示失败。
         * ★ 这里单独计时，用来判断"喂帧只有 62 帧/秒"的瓶颈是不是 feed()。 */
        const int64_t t_feed0 = esp_timer_get_time();
        const int      feed_ret = s_afe->feed(s_afe_data, s_feed_buf);
        s_t_feed_us += (uint32_t)(esp_timer_get_time() - t_feed0);
        s_t_feeds++;
        if (feed_ret < 0) {
            s_st_feed_fail++;
            if ((s_st_feed_fail == 1) || ((s_st_feed_fail % 100) == 0)) {
                ESP_LOGW(TAG, "AFE feed() 失败 %u 次，重置 AFE 环形缓冲",
                         (unsigned)s_st_feed_fail);
            }
            s_afe->reset_buffer(s_afe_data);
        } else {
            s_st_feed_ok++;
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  识别任务：fetch 音频 → 唤醒词 → 命令词 → voice_dispatch()                   */
/* -------------------------------------------------------------------------- */
static void voice_sr_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "识别任务已启动：先喊「%s」，等日志出现『已唤醒』后再说命令词", s_wake_word);
    ESP_LOGI(TAG, "命令词共 %d 条，串口敲 voice-test 可打印完整列表", (int)VOICE_SR_CMD_NUM);

    uint32_t w_frames     = 0;      /* 本窗口内 fetch 成功的帧数 */
    uint32_t w_fetch_null = 0;      /* 本窗口内 fetch 没取到数据的次数 */
    int64_t  last_status_us = esp_timer_get_time();

    ESP_LOGI(TAG, "识别任务进入主循环：阻塞式 fetch，超时 %d ms",
             VOICE_SR_FETCH_TIMEOUT_MS);

    for (;;) {
        /* ---- 1. 取增强后的音频 + 唤醒状态 ----
         * ★ 这里必须给【真实超时】，不能用 0：
         *   0 = 空轮询，AFE 每被问一次空就打一条 "Ringbuffer of AFE is empty"，
         *   实测刷屏 155 行/秒，并且靠串口阻塞反过来拖慢喂帧。
         *   给超时后它会安静地等数据，等不到只返回 NULL。 */
        afe_fetch_result_t *res =
            s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(VOICE_SR_FETCH_TIMEOUT_MS));

        /* ---- 2. 周期性健康日志（每 5 秒一条）----
         * ★ 位置非常关键：必须放在本循环【所有 continue 之前】。
         *   上一版这段写在 fetch 之后、且 fetch 失败就 continue，于是这条
         *   日志一次都没打出来 —— 排查时等于完全没有眼睛。 */
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_status_us > 5LL * 1000 * 1000) {
            const int64_t win_us = now_us - last_status_us;
            i2s_mic_level_t lv;
            i2s_mic_read_level(&lv);
            /* 求两段耗时的均值（探针由喂帧任务累计，这里读取并清零） */
            const uint32_t n_reads     = s_t_reads ? s_t_reads : 1;
            const uint32_t n_feeds     = s_t_feeds ? s_t_feeds : 1;
            const uint32_t avg_read_us = s_t_read_us / n_reads;
            const uint32_t avg_feed_us = s_t_feed_us / n_feeds;
            /* ★ feed 速率与 fetch 速率必须【分开算、各自对标】（这里踩过坑）：
             *   feed  = 真正喂进 AFE 的帧数/秒，目标 = 16000/160 = 100；
             *   fetch = 识别循环取到数据的次数/秒 ≈ feed/3.2
             *           （fetch_chunksize 512 = 3.2 × feed_chunksize 160）。
             *   上一版把 fetch 次数标成"喂帧/秒、目标 100"，于是 62 被误读成
             *   "喂帧不达标、丢了 38% 音频"——两个量根本不是一回事。 */
            const int feed_rate  = (win_us > 0)
                                       ? (int)((int64_t)s_t_feeds * 1000000 / win_us) : 0;
            const int fetch_rate = (win_us > 0)
                                       ? (int)((int64_t)w_frames * 1000000 / win_us) : 0;
            const uint32_t n_detect     = s_diag_detect_n ? s_diag_detect_n : 1;
            const uint32_t avg_detect_us = s_diag_detect_us / n_detect;
            ESP_LOGI(TAG, "[状态] feed %u 帧/%d 帧每秒（目标 %d，每帧 %d 点）"
                          " | fetch %u 次/%d 次每秒（每次 %d 点）"
                          " | 喂失败=%u 全0=%u fetch空=%u"
                          " | 读%u次 feed%u次 占用=%u%% 读均=%u.%02ums feed均=%u.%02ums"
                          " | detect=%u次 均=%u.%02ums"
                          " | rms=%d peak=%d dBFS=%d.%d 就绪=%s 唤醒=%s"
                          " | 栈余 feed=%uB rec=%uB",
                     (unsigned)s_st_feed_ok, feed_rate,
                     BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize, s_feed_chunksize,
                     (unsigned)w_frames, fetch_rate,
                     s_afe->get_fetch_chunksize(s_afe_data),
                     (unsigned)s_st_feed_fail, (unsigned)s_st_zero,
                     (unsigned)w_fetch_null,
                     (unsigned)s_t_reads, (unsigned)s_t_feeds,
                     (unsigned)((win_us > 0)
                                    ? ((int64_t)(s_t_read_us + s_t_feed_us) * 100 / win_us)
                                    : 0),
                     (unsigned)(avg_read_us / 1000), (unsigned)((avg_read_us % 1000) / 10),
                     (unsigned)(avg_feed_us / 1000), (unsigned)((avg_feed_us % 1000) / 10),
                     (unsigned)s_diag_detect_n,
                     (unsigned)(avg_detect_us / 1000), (unsigned)((avg_detect_us % 1000) / 10),
                     lv.rms, lv.peak,
                     lv.db_x10 / 10, (lv.db_x10 < 0 ? -lv.db_x10 : lv.db_x10) % 10,
                     i2s_mic_is_ready() ? "是" : "否", s_awake ? "是" : "否",
                     (unsigned)((s_feed_task_h != NULL)
                                    ? uxTaskGetStackHighWaterMark(s_feed_task_h) * sizeof(StackType_t)
                                    : 0),
                     (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
            last_status_us = now_us;
            w_frames       = 0;
            w_fetch_null   = 0;
            s_t_read_us    = 0;   /* 耗时探针按窗口清零 */
            s_t_reads      = 0;
            s_t_feed_us    = 0;
            s_t_feeds      = 0;
            s_diag_detect_us = 0;
            s_diag_detect_n  = 0;
        }

        if (res == NULL || res->ret_value == -1) {
            w_fetch_null++;
            continue;   /* fetch_with_delay 已经等过了，这里不需要再 delay */
        }
        w_frames++;

        /* ---- 诊断探针（见 s_diag_last_vad 处的说明）---- */
        if ((int32_t)res->vad_state != s_diag_last_vad) {
            s_diag_last_vad = (int32_t)res->vad_state;
            ESP_LOGI(TAG, "[诊断] VAD 状态变为: %s",
                     (res->vad_state == VAD_SPEECH) ? "说话(SPEECH)" : "静音(SILENCE)");
        }
        if (res->wakeup_state != WAKENET_NO_DETECT) {
            ESP_LOGI(TAG, "[诊断] wakenet 状态=%d（1=检测到唤醒词, -1=通道确认）",
                     (int)res->wakeup_state);
        }
        if (res->vad_state == VAD_SPEECH) {
            const int64_t now_diag_us = esp_timer_get_time();
            if (now_diag_us - s_diag_last_vol_us > 1000000LL) {
                s_diag_last_vol_us = now_diag_us;
                /* 统计 fetch 输出帧的峰值：wakenet/MultiNet 拿到的就是这个音频。
                 * 说话时 peak 应到几千；peak≈0 说明 AFE 输出是空的，
                 * 那 data_volume 的 0 就是"真没声音"，不是字段没填充。 */
                int32_t pk = 0;
                const int n = (res->data != NULL) ? (res->data_size / 2) : 0;
                for (int i = 0; i < n; i++) {
                    int32_t a = (int32_t)res->data[i];
                    if (a < 0) { a = -a; }
                    if (a > pk) { pk = a; }
                }
                ESP_LOGI(TAG, "[诊断] VAD=说话 | 输出帧 %d 采样 peak=%d | data_volume=%d(dBx10) | vad_cache=%d",
                         n, (int)pk, (int)(res->data_volume * 10.0f), res->vad_cache_size);
            }
        }

        /* ---- 3. 唤醒词命中 ----
         * wakeup_state 枚举：WAKENET_DETECTED(1) 检测到；WAKENET_CHANNEL_VERIFIED(-1)
         * 是"多麦时确认了是哪个通道"，单麦不会出现，但一起判上更稳。 */
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (!s_awake) {
                s_awake = true;
                /* 唤醒提示：本工程没有喇叭，所以"提示音"靠串口日志 + OLED 弹窗。
                 * （将来加了功放+喇叭，可以在这里播一个"滴"的提示音。） */
                ESP_LOGI(TAG, "★ 已唤醒（唤醒词=「%s」）—— 请说命令词，%d 秒内有效",
                         s_wake_word, VOICE_SR_MN_DURATION_MS / 1000);
                voice_ui_note("已唤醒，请说命令");
            }
            /* 重新开始命令词监听窗口：连续说多条命令时窗口会顺延 */
            s_mn->clean(s_mn_data);
        }

        /* ---- 4. 命令词识别（只在唤醒后跑，这是 MultiNet 的要求） ---- */
        if (s_awake) {
            const int64_t t_det0 = esp_timer_get_time();
            const esp_mn_state_t mn_state = s_mn->detect(s_mn_data, res->data);
            s_diag_detect_us += (uint32_t)(esp_timer_get_time() - t_det0);
            s_diag_detect_n++;

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *r = s_mn->get_results(s_mn_data);
                if (r != NULL && r->num > 0) {
                    /* command_id 就是 voice_cmd_t（注册时故意这么定的） */
                    const int cid = r->command_id[0];
                    /* 概率只有 0~1，用 ×1000 的整数打印，避免用 %f
                     * （本工程可能开 NANO_FORMAT，%f 会打成空串） */
                    const int prob_x1000 = (int)(r->prob[0] * 1000.0f + 0.5f);

                    if (cid > VOICE_CMD_NONE && cid < VOICE_CMD_MAX) {
                        ESP_LOGI(TAG, "命令词命中: 「%s」 (conf=%d.%03d) -> %s",
                                 voice_cmd_name((voice_cmd_t)cid),
                                 prob_x1000 / 1000, prob_x1000 % 1000,
                                 voice_cmd_name((voice_cmd_t)cid));
                        ESP_LOGI(TAG, "识别原文: %s", r->string);

                        /* OLED 反馈：显示识别到的中文命令词 */
                        char ui_buf[48];
                        snprintf(ui_buf, sizeof(ui_buf), "已识别:%s",
                                 voice_sr_cmd_cn((voice_cmd_t)cid));
                        voice_ui_note(ui_buf);

                        /* ★★ 唯一的出口：和 ASRPRO 方案调的是同一个函数 ★★
                         * 上层 main.c::voice_on_cmd()、MQTT、OLED、自动联动
                         * 全都不知道这句话是怎么来的。 */
                        voice_dispatch((voice_cmd_t)cid);
                    } else {
                        ESP_LOGW(TAG, "识别到未知 command_id=%d，忽略", cid);
                    }
                }
                /* 连续命令模式（2026-10-02 用户实测反馈改）：命中一条命令后
                 * 【不】回休眠，重置命令窗口（6 秒内可以继续说下一条），超时
                 * 才回休眠 —— 用户说"开灯、开风扇、拉窗帘"不用每次重喊唤醒词。
                 * 误触发风险：窗口只在真实唤醒+命中后顺延，且命令词表只有 25 条，
                 * 电视/聊天声几乎不会连续命中，可接受。 */
                s_mn->clean(s_mn_data);

            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                ESP_LOGI(TAG, "命令词等待超时（%d 秒），回到休眠，需要重新喊「%s」",
                         VOICE_SR_MN_DURATION_MS / 1000, s_wake_word);
                voice_ui_note("已退出唤醒");
                s_awake = false;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */
esp_err_t voice_esp_sr_start(void)
{
    if (s_started) {
        return ESP_OK;              /* 幂等 */
    }

    ESP_LOGI(TAG, "=========== 启动 ESP-SR 离线语音识别（INMP441 I²S）===========");
    ESP_LOGI(TAG, "⚠ INMP441 是 I²S 不是 I²C：SCK=GPIO%d / WS=GPIO%d / SD=GPIO%d / "
                  "L-R=GND / VDD=3V3",
             (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);

    /* ---- 0. 芯片配置体检（只打警告，不阻断）：CPU 主频/Flash/PSRAM 频率等 ---- */
    check_chip_config();

    /* ---- 1. 麦克风 ---- */
    esp_err_t err = i2s_mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²S 麦克风初始化失败 (%s)：语音识别不可用，"
                      "WiFi/MQTT/BLE/按键/MQTT 控制全部照常工作",
                 esp_err_to_name(err));
        return err;
    }

    /* ---- 2. 加载 model 分区里的模型 ---- */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(\"model\") 失败：model 分区没找到或没烧模型。"
                      "检查分区表里是否有 `model` 分区，以及烧录时是否带了 model 镜像"
                      "（idf.py flash 会自动烧 srmodels.bin）");
        return ESP_ERR_NOT_FOUND;
    }

    err = voice_sr_pick_models();
    if (err != ESP_OK) {
        return err;
    }

    /* ---- 3. 建 AFE（音频前端） ----
     * 输入格式 "M" = 单通道麦克风：
     *   M=麦克风 R=回放参考 N=未用。本工程只有一颗 INMP441、没有喇叭回采，
     *   所以是单 M。参考通道（R）只在做 AEC 回声消除时才需要。 */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init 失败");
        return ESP_FAIL;
    }

    afe_cfg->wakenet_init        = true;
    afe_cfg->wakenet_model_name  = s_wn_name;
    afe_cfg->wakenet_mode        = DET_MODE_90;   /* 90% 触发概率档：比 95% 更容易唤醒，
                                                   * 也更易误唤醒；家居场景先求"喊得动"，
                                                   * 觉得太灵敏再改 DET_MODE_95。 */
    afe_cfg->aec_init            = false;         /* 没有回放参考通道，开 AEC 没意义 */
    afe_cfg->se_init             = false;         /* 单麦没有麦克风阵列波束成形 */
    /* ★ 2026-10-02 实测：开着 NS 时唤醒词极难触发（10 余次喊话 0~1 次命中，
     * 且 esp-sr 自己开机就警告 "Noise Supression may reduce the accuracy
     * of speech recognition"）。关掉 NS 后唤醒显著改善，官方 wakenet 示例
     * 的默认配置本来就不开 NS。家居底噪由 VAD 门控 + wakenet 自身鲁棒性处理。 */
    afe_cfg->ns_init             = false;
    afe_cfg->vad_init            = true;          /* 静音检测：省算力，也帮助切句 */
    afe_cfg->vad_mode            = VAD_MODE_0;    /* 最不激进的 VAD：宁可多留一点音频，
                                                   * 不要把命令词开头吃掉 */
    afe_cfg->afe_mode            = AFE_MODE_HIGH_PERF;
    /* 内存分配：本板是 N16R8（8MB Octal PSRAM，80MHz），AFE 的中间缓冲
     * 放 PSRAM 可以大幅省内部 RAM —— 内部 RAM 要留给 WiFi/BLE 协议栈和
     * 各任务栈，这是"三个无线功能共存"最关键的一条。 */
    afe_cfg->memory_alloc_mode   = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    afe_cfg->afe_perferred_core  = VOICE_SR_TASK_CORE;
    afe_cfg->afe_perferred_priority = VOICE_SR_TASK_PRIO;
    /* 线性增益 1.0 = 不额外放大。INMP441 的灵敏度足够，放大会同时放大底噪，
     * 反而让 VAD 更容易误触发，所以不动它。 */
    afe_cfg->afe_linear_gain     = 1.0f;

    afe_config_check(afe_cfg);      /* 让 esp-sr 自己修正冲突项（比如单麦时的 SE） */
    afe_config_print(afe_cfg);      /* 把最终配置打进日志 —— 排错时第一手资料 */

    s_afe = esp_afe_handle_from_config(afe_cfg);
    if (s_afe == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config 返回 NULL");
        return ESP_FAIL;
    }

    s_afe_data = s_afe->create_from_config(afe_cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "AFE create 失败（多半是内存/PSRAM 不足）");
        return ESP_ERR_NO_MEM;
    }

    /* 配置结构体的使命到此结束：create_from_config() 内部已经把需要的字段
     * 复制/持有，官方示例的写法也是在 create 之后立刻释放。释放掉这一份
     * 可以省下几 KB（内部 RAM 在这个工程里很宝贵）。如果哪天真发现 AFE 还
     * 引用着它，把这两行注释掉即可 —— 最坏也只是泄漏几 KB，不影响功能。 */
    afe_config_free(afe_cfg);
    afe_cfg = NULL;

    s_feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    ESP_LOGI(TAG, "AFE 就绪：feed_chunksize=%d 采样点（%d ms）/ fetch_chunksize=%d / "
                  "采样率 %d Hz / 通道 %d / 目标喂帧 %d 帧/秒",
             s_feed_chunksize,
             (s_feed_chunksize * 1000) / BSP_I2S_MIC_SAMPLE_RATE,
             s_afe->get_fetch_chunksize(s_afe_data),
             s_afe->get_samp_rate(s_afe_data),
             s_afe->get_channel_num(s_afe_data),
             BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize);
    s_afe->print_pipeline(s_afe_data);

    /* feed 缓冲：优先放 PSRAM（内部 RAM 很宝贵）。必须 16bit 对齐，malloc 天然满足。 */
    s_feed_buf = (int16_t *)heap_caps_malloc((size_t)s_feed_chunksize * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_feed_buf == NULL) {
        s_feed_buf = (int16_t *)malloc((size_t)s_feed_chunksize * sizeof(int16_t));
    }
    if (s_feed_buf == NULL) {
        ESP_LOGE(TAG, "feed 缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    /* ---- 4. 建 MultiNet（命令词模型） ---- */
    s_mn = esp_mn_handle_from_name(s_mn_name);
    if (s_mn == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name(\"%s\") 失败", s_mn_name);
        return ESP_FAIL;
    }

    /* duration = 唤醒后等命令词的窗口（毫秒）。超时后 detect 返回 ESP_MN_STATE_TIMEOUT。 */
    s_mn_data = s_mn->create(s_mn_name, VOICE_SR_MN_DURATION_MS);
    if (s_mn_data == NULL) {
        ESP_LOGE(TAG, "MultiNet create 失败（多半是内存/PSRAM 不足）");
        return ESP_ERR_NO_MEM;
    }

    err = voice_sr_register_commands();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "命令词注册失败：语音识别起来了但认不出命令");
        return err;
    }

    /* ---- 5. 起【喂帧】任务 ----
     * ★ 必须和识别任务分成两个任务：喂帧是实时性最高的环节，不能被识别
     *   计算、串口打印、MQTT/BLE 拖慢（详细理由见 voice_sr_feed_task 上方）。 */
    if (xTaskCreatePinnedToCore(voice_sr_feed_task, "voice_feed", VOICE_SR_FEED_STACK, NULL,
                                VOICE_SR_FEED_PRIO, &s_feed_task_h, VOICE_SR_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建喂帧任务失败");
        return ESP_ERR_NO_MEM;
    }

    /* ---- 6. 起【识别】任务 ---- */
    if (xTaskCreatePinnedToCore(voice_sr_task, "voice_sr", VOICE_SR_TASK_STACK, NULL,
                                VOICE_SR_TASK_PRIO, NULL, VOICE_SR_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建识别任务失败");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    s_ready   = true;
    ESP_LOGI(TAG, "=========== ESP-SR 启动成功 ===========");
    ESP_LOGI(TAG, "★ 请喊：「%s」，然后说命令词（如「打开客厅灯」）", s_wake_word);
    ESP_LOGI(TAG, "   完整命令词表：串口敲 voice-test");
    ESP_LOGI(TAG, "   剩余内部堆 %u 字节 / PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

bool voice_esp_sr_is_ready(void)
{
    return s_ready;
}

bool voice_esp_sr_is_awake(void)
{
    return s_awake;
}

const char *voice_esp_sr_wake_word(void)
{
    return s_wake_word[0] ? s_wake_word : "(未启动)";
}

const char *voice_esp_sr_mn_model(void)
{
    return (s_mn_name != NULL) ? s_mn_name : "(未加载)";
}

void voice_esp_sr_print_commands(void)
{
    printf("\n");
    printf("============ ESP-SR 离线语音识别 —— 命令词表 ============\n");
    printf("  唤醒词： 「%s」   （必须先喊它，听到日志 \"已唤醒\" 再说命令）\n",
           s_wake_word[0] ? s_wake_word : "(未启动)");
    printf("  唤醒词模型：%s\n", (s_wn_name != NULL) ? s_wn_name : "(未加载)");
    printf("  命令词模型：%s\n", voice_esp_sr_mn_model());
    printf("  识别状态：%s\n", s_ready ? "已就绪" : "未就绪（看日志里的 VOICE_SR 报错）");
    printf("-------------------------------------------------------\n");
    printf("  %-4s %-14s %-26s %s\n", "序号", "中文命令词", "拼音(实际识别单元)", "对应指令");
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        printf("  %-4u %-14s %-26s %s\n",
               (unsigned)(i + 1), s_cmds[i].cn, s_cmds[i].pinyin,
               voice_cmd_name(s_cmds[i].cmd));
    }
    printf("-------------------------------------------------------\n");
    printf("  ⚠ 本方案只有麦克风、没有喇叭：控制类指令正常执行；\n");
    printf("    查询类指令（温度多少/播报全部…）会走完业务链路但【不会出声】。\n");
    printf("=======================================================\n\n");
}

#else  /* !CONFIG_APP_VOICE_SOURCE_ESP_SR */

/* 开关关闭时：整个文件退化成空实现。
 * 这样 voice.c 里就可以无条件调用 voice_esp_sr_start()（由它返回
 * ESP_ERR_NOT_SUPPORTED），不必在 voice.c 里到处写 #if —— 少一层条件编译，
 * 出问题更好查。而且这一份空实现不需要 esp-sr 头文件，
 * 所以 CMakeLists 里把 esp-sr 挂成条件依赖也是安全的。 */

esp_err_t voice_esp_sr_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

voice_ui_note_t g_voice_ui_note = { 0, { 0 } };

void voice_ui_note(const char *text)
{
    (void)text;   /* ESP-SR 关闭时没有语音事件可提示 */
}

bool voice_esp_sr_is_ready(void)
{
    return false;
}

bool voice_esp_sr_is_awake(void)
{
    return false;
}

const char *voice_esp_sr_wake_word(void)
{
    return "(ESP-SR 未启用)";
}

const char *voice_esp_sr_mn_model(void)
{
    return "(ESP-SR 未启用)";
}

void voice_esp_sr_print_commands(void)
{
    printf("\n  当前固件未启用 ESP-SR（menuconfig → 应用行为 → 语音识别来源）。\n"
           "  现在用的是 ASRPRO UART 方案：say <voice_cmd> 可模拟，help-voice 看指令表。\n\n");
}

#endif /* CONFIG_APP_VOICE_SOURCE_ESP_SR */
