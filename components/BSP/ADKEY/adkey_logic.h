/**
 * @file  adkey_logic.h
 * @brief 五位 AD 键盘的【纯判定逻辑】：ADC 毫伏值 → 哪个键
 *
 * 为什么要单独抽出来：
 *   adkey.c 里混着 ADC 采样、消抖、事件派发，那些都得在板子上跑；
 *   但"这个电压算哪个键"其实是纯计算，抽到这里就能在 **电脑上直接单测**
 *   （见 tools/adkey_test.c），换键盘、调阈值时不用反复烧板子试。
 *
 * 判定顺序（与 adkey.c 的注释一一对应）：
 *   ① mv >= idle_mv - ok_margin  → 无键（ADC 顶端的空闲电平）
 *   ② mv <= ok_low_max           → OK   ★本机 OK 就在这里（模块把 OK 直接对地，≈0mV）
 *   ③ mv >= ok_high_min          → OK   兼容"OK 档贴近 VCC、压降只有几 mV"的其它批次模块
 *   ④ 离 4 个方向键档最近且 ≤ tolerance → 该方向键
 *   ⑤ 其余                       → 无键（调用方会打限速告警，便于现场标定）
 *
 * 历史教训（★ 别再改回去）：
 *   曾经只有 ①③④，没有 ② —— 于是本机按 OK（0mV）时：
 *     · adkey_read_mv() 把 0mV 当成"读失败"（写成 `mv > 0` 判据）；
 *     · adkey_match() 又没有任何一档能容纳 0mV（离最近的 627mV 档差 600mV）。
 *   两个 bug 叠在一起，表现就是「按 OK 毫无反应」。见 docs/11。
 */
#pragma once

/* 与 adkey.h 的 adkey_id_t 顺序保持一致：0..3 = 键1..4，4 = OK，5 = 无键 */
#define ADKEY_LOGIC_NUM   5
#define ADKEY_LOGIC_OK    4
#define ADKEY_LOGIC_NONE  5

/* ------------------------------------------------------------------ */
/*  本机键盘的实测调参（换键盘只改这一段；固件与 PC 单测共用同一份取值）*/
/* ------------------------------------------------------------------ */
#define ADKEY_TOLERANCE_MV      200     /* 方向键匹配容差 ±mV（覆盖分压电阻 ±10%） */
#define ADKEY_OK_LOW_MAX_MV     300     /* ★ OK 低压档上界：0~300mV 都判为 OK
                                         *   （本机 OK 直接对地 ≈0mV；最低的方向键档
                                         *    ≈615mV，间隔充足） */
#define ADKEY_OK_MIN_MV         2900    /* OK 高压残余档下界：兼容"OK 贴近 VCC、
                                         *   压降只有几 mV"的其它批次模块 */
#define ADKEY_OK_MARGIN_MV      3       /* 空闲判定线 = 空闲基线 - 本值 */

/* ★ 高压残余档开关（默认 0 = 关闭）。
 *
 * 2026-09-28 实机验证结论：本机键盘的 OK **确实是 0mV 低压档**，日志为证：
 *     [ADKEY] OK DOWN (触发读数=0mV, 当前=0mV)
 *     ADKEY: 标定: 键OK 按下期间最低 0mV
 *
 * 而那条高压档的代价是【误触发】：同一次实机抓包里，空闲时每隔几秒就冒出一串
 * 只持续 30ms 的假 OK 事件（触发读数 2905~3124mV，明显是 ADC 噪声，人手不可能
 * 30ms 按一次）：
 *     [ADKEY] OK DOWN (触发读数=3121mV) … 30ms 后 OK CLICK
 * 这些假事件会让菜单自己乱跳，所以默认关闭。
 *
 * 什么时候要打开：换成一个"OK 档不在 0mV、而是贴近 VCC"的模块时，
 * 把它置 1（并把 ADKEY_OK_MIN_MV 调成实测值）。
 * 打开前先用 `keyscan` 确认空闲够稳、没有这种几十 mV 的噪声跌落。 */
#define ADKEY_OK_HIGH_ENABLE    0

/* 4 个方向键的实测分压（顺序同 adkey_id_t：键 1/2/3/4）。
 * 换键盘时用 `keyscan` 打印出来的值改这里即可 —— 固件和 PC 单测都读这一份。 */
#define ADKEY_KEY1_MV           1397    /* 丝印 1 = 左，实测 ~1380mV */
#define ADKEY_KEY2_MV           2639    /* 丝印 2 = 右，实测 ~2638mV */
#define ADKEY_KEY3_MV           627     /* 丝印 3 = 上，实测 ~615mV  */
#define ADKEY_KEY4_MV           1971    /* 丝印 4 = 下，实测 ~1968mV */

typedef struct {
    unsigned short key_mv[4];   /**< 4 个方向键的实测分压（顺序同 adkey_id_t：键1/2/3/4） */
    int tolerance;              /**< 方向键匹配容差 ±mV（覆盖分压电阻 ±10%） */
    int ok_low_max;             /**< OK 低压档上界（0~该值 判为 OK） */
    int ok_high_min;            /**< OK 高压残余档下界 */
    int ok_high_enable;         /**< 是否启用高压残余档（本机实测 0mV，默认关闭防误触发） */
    int ok_margin;              /**< 空闲判定线 = idle_mv - ok_margin */
} adkey_logic_cfg_t;

/**
 * @brief 电压 → 键
 * @param mv        ADC 读数（mV）；<0 视为无效（当作无键）
 * @param idle_mv   开机自校准的空闲基线（mV）
 * @param cfg       阈值配置
 * @return 0..3 = 键1..4；ADKEY_LOGIC_OK = OK；ADKEY_LOGIC_NONE = 无按键
 */
static inline int adkey_logic_match(int mv, int idle_mv, const adkey_logic_cfg_t *cfg)
{
    /* ① 空闲：判定线跟着开机基线走（基线漂多少，判定线跟着漂多少） */
    if (mv < 0 || mv >= idle_mv - cfg->ok_margin) {
        return ADKEY_LOGIC_NONE;
    }

    /* ② ★ 低压档 = OK（本机实测 OK 直接对地 ≈0mV）—— "按 OK 没反应"的修复点 */
    if (mv <= cfg->ok_low_max) {
        return ADKEY_LOGIC_OK;
    }

    /* ③ 高压残余档 = OK（默认关闭：本机实测会误触发，见 ADKEY_OK_HIGH_ENABLE 说明） */
    if (cfg->ok_high_enable && mv >= cfg->ok_high_min) {
        return ADKEY_LOGIC_OK;
    }

    /* ④ 最近邻匹配 4 个方向键（容差含端点：正好 ±tolerance 也算命中） */
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
    return best;   /* ⑤ 落不进任何档 → NONE */
}
