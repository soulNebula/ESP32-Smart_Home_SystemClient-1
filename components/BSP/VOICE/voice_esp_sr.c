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
// 认出的命令从这走
#include "voice_internal.h"

#if CONFIG_APP_VOICE_SOURCE_ESP_SR

// 识别用的头文件
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
// 查芯片配置用
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

// 查有没有挑模型
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


static const char *TAG = "VOICE_SR";

// 这块用的死数
// 认话任务栈
#define VOICE_SR_TASK_STACK     12288
// 认话的优先级
#define VOICE_SR_TASK_PRIO      7
// 都绑在核一
#define VOICE_SR_TASK_CORE      1

// 等命令的窗口
#define VOICE_SR_MN_DURATION_MS 6000
// 读一次等多久
#define VOICE_SR_READ_TIMEOUT_MS 1000

// 喂声音单独一个任务
// 喂声音任务栈
#define VOICE_SR_FEED_STACK      12288
// 喂声音的优先级
#define VOICE_SR_FEED_PRIO       5

// 取结果最多等这么久
#define VOICE_SR_FETCH_TIMEOUT_MS 100

// 说中文是哪条命令
typedef struct {
    // 中文说法，给人看
    const char *cn;
    // 交给识别的拼音
    const char *pinyin;
    // 翻成哪条命令
    voice_cmd_t cmd;
} voice_sr_cmd_t;

static const voice_sr_cmd_t s_cmds[] = {
    // 各个房间的灯
    { "打开客厅灯",   "da kai ke ting deng",     VOICE_CMD_LED_LIVING_ON    },
    { "关闭客厅灯",   "guan bi ke ting deng",    VOICE_CMD_LED_LIVING_OFF   },
    { "打开厨房灯",   "da kai chu fang deng",    VOICE_CMD_LED_KITCHEN_ON   },
    { "关闭厨房灯",   "guan bi chu fang deng",   VOICE_CMD_LED_KITCHEN_OFF  },
    { "打开卧室灯",   "da kai wo shi deng",      VOICE_CMD_LED_BEDROOM_ON   },
    { "关闭卧室灯",   "guan bi wo shi deng",     VOICE_CMD_LED_BEDROOM_OFF  },
    { "打开浴室灯",   "da kai yu shi deng",      VOICE_CMD_LED_BATH_ON      },
    { "关闭浴室灯",   "guan bi yu shi deng",     VOICE_CMD_LED_BATH_OFF     },

    // 所有的灯
    { "打开全部灯",   "da kai quan bu deng",     VOICE_CMD_LED_ALL_ON       },
    { "关闭全部灯",   "guan bi quan bu deng",    VOICE_CMD_LED_ALL_OFF      },

    // 风扇
    { "打开风扇",     "da kai feng shan",        VOICE_CMD_FAN_ON           },
    { "关闭风扇",     "guan bi feng shan",       VOICE_CMD_FAN_OFF          },

    // 窗户
    { "打开窗户",     "da kai chuang hu",        VOICE_CMD_WINDOW_OPEN      },
    { "关闭窗户",     "guan bi chuang hu",       VOICE_CMD_WINDOW_CLOSE     },

    // 门
    { "打开门",       "da kai men",              VOICE_CMD_DOOR_OPEN        },
    { "关上门",       "guan shang men",          VOICE_CMD_DOOR_CLOSE       },

    // 窗帘
    { "拉开窗帘",     "la kai chuang lian",      VOICE_CMD_CURTAIN_OPEN     },
    { "拉上窗帘",     "la shang chuang lian",    VOICE_CMD_CURTAIN_CLOSE    },

    // 问一句
    { "温度多少",     "wen du duo shao",         VOICE_CMD_QUERY_TEMP       },
    { "湿度多少",     "shi du duo shao",         VOICE_CMD_QUERY_HUMI       },
    { "光照多少",     "guang zhao duo shao",     VOICE_CMD_QUERY_LIGHT      },
    { "播报全部",     "bao bao quan bu",         VOICE_CMD_QUERY_ALL        },
    { "状态如何",     "zhuang tai ru he",        VOICE_CMD_QUERY_STATUS     },

    // 自动联动的开关
    { "打开自动",     "da kai zi dong",          VOICE_CMD_AUTO_ON          },
    { "关闭自动",     "guan bi zi dong",         VOICE_CMD_AUTO_OFF         },
};

#define VOICE_SR_CMD_NUM (sizeof(s_cmds) / sizeof(s_cmds[0]))

// 记着跑到哪一步
// 开过没，防重开
static bool              s_started   = false;
// 全备齐了没
static volatile bool     s_ready     = false;
// 喊醒了没
static volatile bool     s_awake     = false;

// 板子上的模型
static srmodel_list_t   *s_models       = NULL;
// 唤醒词的模型
static char             *s_wn_name      = NULL;
// 命令词的模型
static char             *s_mn_name      = NULL;
// 要喊那句话
static char              s_wake_word[64] = { 0 };

static const esp_afe_sr_iface_t *s_afe      = NULL;
static esp_afe_sr_data_t        *s_afe_data = NULL;
static int                       s_feed_chunksize = 0;

static const esp_mn_iface_t *s_mn      = NULL;
static model_iface_data_t   *s_mn_data = NULL;

// 喂声音的中转
static int16_t *s_feed_buf = NULL;

// 挑出要用的模型
static esp_err_t voice_sr_pick_models(void)
{
    if (s_models == NULL) {
        ESP_LOGE(TAG, "模型列表为空，无法挑选模型");
        return ESP_ERR_INVALID_STATE;
    }

    // 先打出模型名字
    ESP_LOGI(TAG, "model 分区里共有 %d 个模型：", s_models->num);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, s_models->model_name[i]);
    }

    // 挑唤醒词的模型
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

    // 挑中文命令词模型
    s_mn_name = esp_srmodel_filter(s_models, "mn", ESP_MN_CHINESE);
    if (s_mn_name == NULL) {
        s_mn_name = esp_srmodel_filter(s_models, "mn", NULL);
    }
    if (s_mn_name == NULL) {
        ESP_LOGE(TAG, "model 分区里找不到命令词模型（mn*）—— "
                      "检查 menuconfig → ESP Speech Recognition → Chinese Speech Commands Model");
        return ESP_ERR_NOT_FOUND;
    }

    // 读出要喊那句话
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

// 把命令词交上去
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
        // 词要交拼音
        const esp_err_t e = esp_mn_commands_add((int)s_cmds[i].cmd, s_cmds[i].pinyin);
        if (e == ESP_OK) {
            ok++;
        } else {
            bad++;
            ESP_LOGW(TAG, "命令词注册失败：%s (%s) -> %s",
                     s_cmds[i].cn, s_cmds[i].pinyin, esp_err_to_name(e));
        }
    }

    // 交上去才算装好
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

// 只给日志看的数
// 喂进去多少帧
static volatile uint32_t s_st_feed_ok   = 0;
// 喂失败多少次
static volatile uint32_t s_st_feed_fail = 0;
// 全零帧多少个
static volatile uint32_t s_st_zero      = 0;

// 留着喂声音的把手
static TaskHandle_t      s_feed_task_h  = NULL;

// 量一量时间花在哪
// 读麦克风累计
static volatile uint32_t s_t_read_us = 0;
// 读了几次
static volatile uint32_t s_t_reads   = 0;
// 喂进去累计
static volatile uint32_t s_t_feed_us = 0;
// 喂了几次
static volatile uint32_t s_t_feeds   = 0;

// 喊不醒时看诊断
// 上次静音还是说话
static volatile int32_t s_diag_last_vad     = -1;
// 电平日志限速
static int64_t          s_diag_last_vol_us  = 0;
// 辨命令累计
static volatile uint32_t s_diag_detect_us   = 0;
// 辨了几次
static volatile uint32_t s_diag_detect_n    = 0;

// 往屏幕递的字
voice_ui_note_t g_voice_ui_note = { 0, { 0 } };

// 写一条屏幕提示
void voice_ui_note(const char *text)
{
    // 先写字再立旗
    snprintf(g_voice_ui_note.text, sizeof(g_voice_ui_note.text), "%s", text);
    g_voice_ui_note.pending = 1;
}

// 命令翻中文说法
static const char *voice_sr_cmd_cn(voice_cmd_t cmd)
{
    for (size_t i = 0; i < VOICE_SR_CMD_NUM; i++) {
        if (s_cmds[i].cmd == cmd) {
            return s_cmds[i].cn;
        }
    }
    return "未知指令";
}

// 专门喂声音的任务
static void voice_sr_feed_task(void *arg)
{
    (void)arg;

    // 先等麦克风稳一稳
    vTaskDelay(pdMS_TO_TICKS(300));

    int64_t last_zero_warn_us = 0;

    ESP_LOGI(TAG, "喂帧任务启动：每帧 %d 采样点（%d ms @%dHz），目标 %d 帧/秒",
             s_feed_chunksize,
             (s_feed_chunksize * 1000) / BSP_I2S_MIC_SAMPLE_RATE,
             BSP_I2S_MIC_SAMPLE_RATE,
             BSP_I2S_MIC_SAMPLE_RATE / s_feed_chunksize);

    for (;;) {
        // 一次读够一帧
        const int64_t t_read0 = esp_timer_get_time();
        size_t got = 0;
        const esp_err_t err = i2s_mic_read(s_feed_buf, (size_t)s_feed_chunksize,
                                           &got, VOICE_SR_READ_TIMEOUT_MS);
        s_t_read_us += (uint32_t)(esp_timer_get_time() - t_read0);
        s_t_reads++;

        if (got < (size_t)s_feed_chunksize) {
            // 没读全就不喂
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

        // 看看是不是全零
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

        // 把这一帧喂进去
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

// 等数据认话交出去
static void voice_sr_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "识别任务已启动：先喊「%s」，等日志出现『已唤醒』后再说命令词", s_wake_word);
    ESP_LOGI(TAG, "命令词共 %d 条，串口敲 voice-test 可打印完整列表", (int)VOICE_SR_CMD_NUM);

    // 这轮取到多少帧
    uint32_t w_frames     = 0;
    // 这轮空手多少次
    uint32_t w_fetch_null = 0;
    int64_t  last_status_us = esp_timer_get_time();

    ESP_LOGI(TAG, "识别任务进入主循环：阻塞式 fetch，超时 %d ms",
             VOICE_SR_FETCH_TIMEOUT_MS);

    for (;;) {
        // 等一小段声音
        afe_fetch_result_t *res =
            s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(VOICE_SR_FETCH_TIMEOUT_MS));

        // 五秒打一条健康日志
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_status_us > 5LL * 1000 * 1000) {
            const int64_t win_us = now_us - last_status_us;
            i2s_mic_level_t lv;
            i2s_mic_read_level(&lv);
            // 算两段平均耗时
            const uint32_t n_reads     = s_t_reads ? s_t_reads : 1;
            const uint32_t n_feeds     = s_t_feeds ? s_t_feeds : 1;
            const uint32_t avg_read_us = s_t_read_us / n_reads;
            const uint32_t avg_feed_us = s_t_feed_us / n_feeds;
            // 喂和取分开算
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
            // 分段的数用完清零
            s_t_read_us    = 0;
            s_t_reads      = 0;
            s_t_feed_us    = 0;
            s_t_feeds      = 0;
            s_diag_detect_us = 0;
            s_diag_detect_n  = 0;
        }

        if (res == NULL || res->ret_value == -1) {
            w_fetch_null++;
            // 等过了，不用再等
            continue;
        }
        w_frames++;

        // 喊不醒时看诊断
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
                // 量声音有多大
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

        // 听见唤醒词了
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (!s_awake) {
                s_awake = true;
                // 喊醒了得让人知道
                ESP_LOGI(TAG, "★ 已唤醒（唤醒词=「%s」）—— 请说命令词，%d 秒内有效",
                         s_wake_word, VOICE_SR_MN_DURATION_MS / 1000);
                voice_ui_note("已唤醒，请说命令");
            }
            // 重开等命令窗口
            s_mn->clean(s_mn_data);
        }

        // 认命令，先喊醒才认
        if (s_awake) {
            const int64_t t_det0 = esp_timer_get_time();
            const esp_mn_state_t mn_state = s_mn->detect(s_mn_data, res->data);
            s_diag_detect_us += (uint32_t)(esp_timer_get_time() - t_det0);
            s_diag_detect_n++;

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *r = s_mn->get_results(s_mn_data);
                if (r != NULL && r->num > 0) {
                    // 命令号就是命令
                    const int cid = r->command_id[0];
                    // 把握用整数打
                    const int prob_x1000 = (int)(r->prob[0] * 1000.0f + 0.5f);

                    if (cid > VOICE_CMD_NONE && cid < VOICE_CMD_MAX) {
                        ESP_LOGI(TAG, "命令词命中: 「%s」 (conf=%d.%03d) -> %s",
                                 voice_cmd_name((voice_cmd_t)cid),
                                 prob_x1000 / 1000, prob_x1000 % 1000,
                                 voice_cmd_name((voice_cmd_t)cid));
                        ESP_LOGI(TAG, "识别原文: %s", r->string);

                        // 屏幕上显示认出了啥
                        char ui_buf[48];
                        snprintf(ui_buf, sizeof(ui_buf), "已识别:%s",
                                 voice_sr_cmd_cn((voice_cmd_t)cid));
                        voice_ui_note(ui_buf);

                        // 唯一的出口
                        voice_dispatch((voice_cmd_t)cid);
                    } else {
                        ESP_LOGW(TAG, "识别到未知 command_id=%d，忽略", cid);
                    }
                }
                // 认完接着听下一条
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

// 把整套识别拉起来
esp_err_t voice_esp_sr_start(void)
{
    if (s_started) {
        // 开过就直接走
        return ESP_OK;
    }

    ESP_LOGI(TAG, "=========== 启动 ESP-SR 离线语音识别（INMP441 I²S）===========");
    ESP_LOGI(TAG, "⚠ INMP441 是 I²S 不是 I²C：SCK=GPIO%d / WS=GPIO%d / SD=GPIO%d / "
                  "L-R=GND / VDD=3V3",
             (int)BSP_I2S_MIC_SCK_GPIO, (int)BSP_I2S_MIC_WS_GPIO, (int)BSP_I2S_MIC_SD_GPIO);

    // 先体检一下板子
    check_chip_config();

    // 把麦克风准备好
    esp_err_t err = i2s_mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²S 麦克风初始化失败 (%s)：语音识别不可用，"
                      "WiFi/MQTT/BLE/按键/MQTT 控制全部照常工作",
                 esp_err_to_name(err));
        return err;
    }

    // 把模型读出来
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

    // 搭好处理流水线
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init 失败");
        return ESP_FAIL;
    }

    afe_cfg->wakenet_init        = true;
    afe_cfg->wakenet_model_name  = s_wn_name;
    // 唤醒门槛调松点
    afe_cfg->wakenet_mode        = DET_MODE_90;
    // 没有回采不用消回声
    afe_cfg->aec_init            = false;
    // 一颗麦不用定向
    afe_cfg->se_init             = false;
    // 关掉降噪
    afe_cfg->ns_init             = false;
    // 分清说没说话
    afe_cfg->vad_init            = true;
    // 判得松一点
    afe_cfg->vad_mode            = VAD_MODE_0;
    afe_cfg->afe_mode            = AFE_MODE_HIGH_PERF;
    // 缓冲放外部
    afe_cfg->memory_alloc_mode   = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    afe_cfg->afe_perferred_core  = VOICE_SR_TASK_CORE;
    afe_cfg->afe_perferred_priority = VOICE_SR_TASK_PRIO;
    // 不额外放大
    afe_cfg->afe_linear_gain     = 1.0f;

    // 让它自己修冲突项
    afe_config_check(afe_cfg);
    // 最终配置打进日志
    afe_config_print(afe_cfg);

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

    // 配置表用完还回去
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

    // 喂声音用的缓冲
    s_feed_buf = (int16_t *)heap_caps_malloc((size_t)s_feed_chunksize * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_feed_buf == NULL) {
        s_feed_buf = (int16_t *)malloc((size_t)s_feed_chunksize * sizeof(int16_t));
    }
    if (s_feed_buf == NULL) {
        ESP_LOGE(TAG, "feed 缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    // 把认命令那块建起来
    s_mn = esp_mn_handle_from_name(s_mn_name);
    if (s_mn == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name(\"%s\") 失败", s_mn_name);
        return ESP_FAIL;
    }

    // 等命令的窗口时长
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

    // 起喂声音的任务
    if (xTaskCreatePinnedToCore(voice_sr_feed_task, "voice_feed", VOICE_SR_FEED_STACK, NULL,
                                VOICE_SR_FEED_PRIO, &s_feed_task_h, VOICE_SR_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "创建喂帧任务失败");
        return ESP_ERR_NO_MEM;
    }

    // 起认话的任务
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

// 看识别起来没
bool voice_esp_sr_is_ready(void)
{
    return s_ready;
}

// 看在不在等命令
bool voice_esp_sr_is_awake(void)
{
    return s_awake;
}

// 报要喊那句话
const char *voice_esp_sr_wake_word(void)
{
    return s_wake_word[0] ? s_wake_word : "(未启动)";
}

// 报命令词模型
const char *voice_esp_sr_mn_model(void)
{
    return (s_mn_name != NULL) ? s_mn_name : "(未加载)";
}

// 把命令表打出来
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

// !CONFIG_APP_VOICE_SOURCE_ESP_SR
#else

// 开关关着时全是空壳

// 没开就直说
esp_err_t voice_esp_sr_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

voice_ui_note_t g_voice_ui_note = { 0, { 0 } };

// 没开就什么都不写
void voice_ui_note(const char *text)
{
    // 没事件可提示
    (void)text;
}

// 没开就是没就绪
bool voice_esp_sr_is_ready(void)
{
    return false;
}

// 没开就不会醒
bool voice_esp_sr_is_awake(void)
{
    return false;
}

// 没开就没有唤醒词
const char *voice_esp_sr_wake_word(void)
{
    return "(ESP-SR 未启用)";
}

// 没开就没有模型
const char *voice_esp_sr_mn_model(void)
{
    return "(ESP-SR 未启用)";
}

// 没开就提示一句
void voice_esp_sr_print_commands(void)
{
    printf("\n  当前固件未启用 ESP-SR（menuconfig → 应用行为 → 语音识别来源）。\n"
           "  现在用的是 ASRPRO UART 方案：say <voice_cmd> 可模拟，help-voice 看指令表。\n\n");
}

#endif
