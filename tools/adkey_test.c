/*
 * 模块：
 *   五位键盘的判键单测。在电脑上跑，不用板子也不用 ESP-IDF，
 *   直接引固件那份阈值表，所以测的就是板上跑的那套判定。
 *   是 tools/run_adkey_test.ps1 把本文件编出来运行的；
 *   配套的实机验证是 tools/verify_adkey.ps1。
 *   编译器打不开带中文的路径，所以脚本先把本文件和 adkey_logic.h 两个文件
 *   拷到英文目录，编译时再用 -I 指过去，绕开编译器的死活路径。
 *
 * 功能：
 *   试五个键准不准
 *   试边界和噪声
 *   试旧逻辑的老毛病
 */
#include <stdio.h>
#include <string.h>

/* 功能：拷到英文目录再编 */
#include "adkey_logic.h"

/* 功能：出厂的那套阈值 */
static const adkey_logic_cfg_t CFG = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = ADKEY_OK_HIGH_ENABLE,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

/* 功能：备用那套，开了会误触 */
static const adkey_logic_cfg_t CFG_HIGH = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = 1,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

/* 功能：手没按时量到的值 */
#define IDLE_MV 3128

static int g_pass = 0;
static int g_fail = 0;

static const char *key_name(int r)
{
    switch (r) {
    case 0: return "键1(左)";
    case 1: return "键2(右)";
    case 2: return "键3(上)";
    case 3: return "键4(下)";
    case ADKEY_LOGIC_OK:   return "OK";
    default:               return "无按键";
    }
}

/* 功能：修改前的老判法 */
static int old_match(int mv, int idle_mv)
{
    if (mv < 0 || mv >= idle_mv - ADKEY_OK_MARGIN_MV) return ADKEY_LOGIC_NONE;
    if (mv >= ADKEY_OK_MIN_MV)                        return ADKEY_LOGIC_OK;
    int best = ADKEY_LOGIC_NONE, min_diff = ADKEY_TOLERANCE_MV;
    for (int i = 0; i < 4; i++) {
        int d = mv - (int)CFG.key_mv[i];
        if (d < 0) d = -d;
        if (d < min_diff) { min_diff = d; best = i; }
    }
    return best;
}

static void expect_cfg(const adkey_logic_cfg_t *cfg, const char *what, int mv, int want)
{
    const int got = adkey_logic_match(mv, IDLE_MV, cfg);
    const int ok  = (got == want);
    if (ok) g_pass++; else g_fail++;
    printf("  [%s] %-40s %4dmV -> %-9s (期望 %s)\n",
           ok ? "OK" : "FAIL", what, mv, key_name(got), key_name(want));
}

static void expect(const char *what, int mv, int want) { expect_cfg(&CFG, what, mv, want); }

int main(void)
{
    printf("=== adkey 判键逻辑单测 ===\n");
    printf("    容差 ±%d mV；OK 低压档 <= %d mV；OK 高压残余档 = %s（阈值 %d mV）；空闲线 = idle-%d\n",
           ADKEY_TOLERANCE_MV, ADKEY_OK_LOW_MAX_MV,
           ADKEY_OK_HIGH_ENABLE ? "开" : "关", ADKEY_OK_MIN_MV, ADKEY_OK_MARGIN_MV);
    printf("    方向键基准: 键1=%d 键2=%d 键3=%d 键4=%d mV；空闲基线 = %d mV\n\n",
           ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV, IDLE_MV);

    printf("-- 1) 本机实测值（logs/monitor-20260928-2053*.log）--\n");
    expect("空闲",                            IDLE_MV, ADKEY_LOGIC_NONE);
    expect("OK（直接对地，★本次修复重点）",   0,       ADKEY_LOGIC_OK);
    expect("OK 接触电阻略抬起",               50,      ADKEY_LOGIC_OK);
    expect("OK 抬起较多",                     200,     ADKEY_LOGIC_OK);
    expect("键3 = 上",                        615,     2);
    expect("键1 = 左",                        1380,    0);
    expect("键4 = 下",                        1968,    3);
    expect("键2 = 右",                        2638,    1);

    printf("\n-- 2) 边界 / 容差 / 无效值 --\n");
    expect("空闲判定线（idle-3，仍算空闲）",  IDLE_MV - ADKEY_OK_MARGIN_MV, ADKEY_LOGIC_NONE);
    expect("判定线下一格（噪声区，不该当 OK）", IDLE_MV - ADKEY_OK_MARGIN_MV - 1, ADKEY_LOGIC_NONE);
    expect("低压档上界",                      ADKEY_OK_LOW_MAX_MV,     ADKEY_LOGIC_OK);
    expect("越过低压档、又够不到方向键",      ADKEY_OK_LOW_MAX_MV + 1, ADKEY_LOGIC_NONE);
    expect("键3 容差端点",                    ADKEY_KEY3_MV + ADKEY_TOLERANCE_MV, 2);
    expect("键3 容差再外一格（落空）",        ADKEY_KEY3_MV + ADKEY_TOLERANCE_MV + 1, ADKEY_LOGIC_NONE);
    expect("读失败（-1）当作空闲",            -1, ADKEY_LOGIC_NONE);

    printf("\n-- 3) ★实机噪声抗扰：这些值来自 2026-09-28 实机抓包里的\"假 OK\"，必须判成无按键 --\n");
    expect("假OK 一次（最短）",  3124, ADKEY_LOGIC_NONE);
    expect("假OK",               3121, ADKEY_LOGIC_NONE);
    expect("假OK",               3115, ADKEY_LOGIC_NONE);
    expect("假OK",               3086, ADKEY_LOGIC_NONE);
    expect("假OK",               3072, ADKEY_LOGIC_NONE);
    expect("假OK",               2982, ADKEY_LOGIC_NONE);
    expect("假OK（最深的一次）", 2905, ADKEY_LOGIC_NONE);

    printf("\n-- 4) 兼容模式：强制打开高压残余档（换模块才用，说明会误触发）--\n");
    expect_cfg(&CFG_HIGH, "高压档打开后 3100mV 会被当成 OK", 3100, ADKEY_LOGIC_OK);
    expect_cfg(&CFG_HIGH, "高压档打开后 OK(0mV) 仍然正常",    0,    ADKEY_LOGIC_OK);
    expect_cfg(&CFG_HIGH, "高压档打开后方向键不受影响",       615,  2);

    printf("\n-- 5) 旧逻辑对照（复现 bug，证明修复有效）--\n");
    struct { int mv; const char *what; } cases[] = {
        { 0,    "按 OK（0mV）" },
        { 50,   "按 OK（50mV）" },
        { 615,  "按 键3" },
        { 1380, "按 键1" },
        { 2638, "按 键2" },
    };
    int fixed_cnt = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        const int o = old_match(cases[i].mv, IDLE_MV);
        const int n = adkey_logic_match(cases[i].mv, IDLE_MV, &CFG);
        const int improved = (o != n);
        if (improved) fixed_cnt++;
        printf("  %-18s %4dmV :  旧=%-9s 新=%-9s %s\n",
               cases[i].what, cases[i].mv, key_name(o), key_name(n),
               improved ? "  ← 修好了" : "");
    }
    printf("  小结：旧逻辑把 OK 判成「无按键」，新逻辑正确识别为 OK。\n");
    if (fixed_cnt != 2) { printf("  [FAIL] 预期恰好 2 个用例被修复\n"); g_fail++; }
    else                { g_pass++; }

    printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
