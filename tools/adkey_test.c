/**
 * @file  adkey_test.c
 * @brief 五位 AD 键盘【判键逻辑】的 PC 端单测（不需要开发板、不需要 ESP-IDF）
 *
 * 直接 include 固件用的那份 adkey_logic.h，所以测的就是板上跑的那套阈值。
 * 编译运行见 tools/run_adkey_test.ps1（用 MinGW gcc）。
 *
 * 用例分五类：
 *   1) 本机实测值 —— 四个方向键 + OK(0mV) + 空闲(3128mV) 必须全部命中；
 *   2) 边界与容差 —— 判定线、低压档上沿、容差端点、无效读数；
 *   3) ★ 实机噪声抗扰 —— 2026-09-28 实机抓包里那批"假 OK"电压（2905~3124mV）
 *      必须全部判成"无按键"，否则菜单会自己乱跳；
 *   4) 兼容模式 —— 打开高压残余档后的行为（换模块才用）；
 *   5) 旧逻辑对照 —— 复现"按 OK 无反应"的 bug，证明修复确实解决了它。
 *
 * 阈值来源：components/BSP/ADKEY/adkey_logic.h（固件与单测共用同一份宏）。
 */
#include <stdio.h>
#include <string.h>

/* 编译时由 tools\run_adkey_test.ps1 把 adkey_logic.h 一起拷到 ASCII 暂存目录，
 * 再用 -I 指过去（MinGW gcc 对含中文的路径会抓瞎，所以不能直接编译工程内路径）。*/
#include "adkey_logic.h"

/* ── 出厂配置：与 adkey.c 的 s_match_cfg 完全同源（都用 adkey_logic.h 的宏）── */
static const adkey_logic_cfg_t CFG = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = ADKEY_OK_HIGH_ENABLE,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

/* ── 兼容模式：强制打开高压残余档（换"OK 贴近 VCC"的模块时才这么配）── */
static const adkey_logic_cfg_t CFG_HIGH = {
    .key_mv         = { ADKEY_KEY1_MV, ADKEY_KEY2_MV, ADKEY_KEY3_MV, ADKEY_KEY4_MV },
    .tolerance      = ADKEY_TOLERANCE_MV,
    .ok_low_max     = ADKEY_OK_LOW_MAX_MV,
    .ok_high_min    = ADKEY_OK_MIN_MV,
    .ok_high_enable = 1,
    .ok_margin      = ADKEY_OK_MARGIN_MV,
};

/* 本机实测的空闲基线（开机自校准得到，实机日志里一直是 3128） */
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

/* ---- 旧逻辑（修复前）：只匹配 4 个方向键 + 高压残余档（且无低压档）---- */
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
