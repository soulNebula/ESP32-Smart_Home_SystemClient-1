/*
 * 模块：
 *   键盘的判定规则。只看电压认是哪个键，跟板子没关系，所以能拿到电脑上单测；
 *   被 adkey.c 调用，测试程序在 tools/adkey_test.c。
 *
 * 功能：
 *   看电压落在哪一档
 *   离空闲太近就不算按
 *   最低那一档算 OK
 */
#pragma once

/* 功能：顺序跟 adkey.h 一样 */
#define ADKEY_LOGIC_NUM   5
#define ADKEY_LOGIC_OK    4
#define ADKEY_LOGIC_NONE  5

/* 功能：换键盘只改这一段 */
#define ADKEY_TOLERANCE_MV      200     /* 功能：认键的容差范围 */
#define ADKEY_OK_LOW_MAX_MV     300     /* 功能：0 伏附近算 OK */
#define ADKEY_OK_MIN_MV         2900    /* 功能：贴近满压也算 OK */
#define ADKEY_OK_MARGIN_MV      3       /* 功能：离太近就当空闲 */

/* 功能：高压那档默认关掉 */
#define ADKEY_OK_HIGH_ENABLE    0

/* 功能：方向键的实测电压 */
#define ADKEY_KEY1_MV           1397    /* 功能：丝印 1 是左键 */
#define ADKEY_KEY2_MV           2639    /* 功能：丝印 2 是右键 */
#define ADKEY_KEY3_MV           627     /* 功能：丝印 3 是上键 */
#define ADKEY_KEY4_MV           1971    /* 功能：丝印 4 是下键 */

typedef struct {
    unsigned short key_mv[4];   /* 功能：方向键的实测电压 */
    int tolerance;              /* 功能：认键的容差范围 */
    int ok_low_max;             /* 功能：低于它算 OK */
    int ok_high_min;            /* 功能：高于它也算 OK */
    int ok_high_enable;         /* 功能：这一档开不开 */
    int ok_margin;              /* 功能：空闲判定线差多少 */
} adkey_logic_cfg_t;

/* 功能：看电压认是哪个键 */
static inline int adkey_logic_match(int mv, int idle_mv, const adkey_logic_cfg_t *cfg)
{
    /* 功能：跟空闲挨着算没按 */
    if (mv < 0 || mv >= idle_mv - cfg->ok_margin) {
        return ADKEY_LOGIC_NONE;
    }

    /* 功能：最低档就是 OK */
    if (mv <= cfg->ok_low_max) {
        return ADKEY_LOGIC_OK;
    }

    /* 功能：贴近满压也算 OK */
    if (cfg->ok_high_enable && mv >= cfg->ok_high_min) {
        return ADKEY_LOGIC_OK;
    }

    /* 功能：挑离得最近的那个键 */
    int best     = ADKEY_LOGIC_NONE;
    int min_diff = cfg->tolerance;
    for (int i = 0; i < 4; i++) {
        int diff = mv - (int)cfg->key_mv[i];
        if (diff < 0) {
            diff = -diff;
        }
        if (diff <= min_diff) {
            min_diff = diff;
            best     = i;
        }
    }
    return best;   /* 功能：落不进档就算没按 */
}
