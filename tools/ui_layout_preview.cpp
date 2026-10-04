/**
 * @file  ui_layout_preview.cpp
 * @brief 128x64 OLED 排版的【PC 端预览 + 重叠检测】工具（不用板子、不用烧录）
 *
 * 为什么需要它：
 *   本工程最小的中文字体是 wqy12（行高 ≈13px），10x20 大字号行高 ≈21px，
 *   64px 高的屏只放得下 4 行。凭手感给 y 坐标极易把两行叠在一起 —— 2026-09-28
 *   主页就出过这个事故（y=49/58/62 三行挤成一团，除温湿度外全糊）。
 *   这个工具用【和固件同一份 astra::config 行基线】+【同一批 u8g2 字模】
 *   在 PC 上把屏幕渲染成 ASCII 图，并逐元素算出墨迹包围盒、报告行间重叠，
 *   所以改排版前可以先在这里看一眼，再决定要不要烧板子。
 *
 * 编译运行： tools/run_ui_preview.ps1（MinGW g++ + u8g2 源码）
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "u8g2.h"          /* -I <stage>/u8g2   （拷自 components/u8g2/csrc） */
}

/* ★ 和固件共用同一份配置（行基线 rowTitleY/rowBigY/row3Y/row4Y 就在里面）
 *   -I <stage>/cfg ，拷自 components/astra_ui/astra/config/config.h */
#include "config.h"

#define SCR_W 128
#define SCR_H 64
#define SCR_TILES (SCR_H / 8)

static u8g2_t  g_u8g2;
static uint8_t *g_buf = nullptr;   /* 指向 u8g2 内部缓冲（setup 之后才有效） */

/* 空的 u8x8 回调：我们只把画面画进 RAM，不真的发 I2C */
static uint8_t u8x8_dummy_msg(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 1;
}

static void screen_clear(void)
{
    u8g2_ClearBuffer(&g_u8g2);
}

static bool px(int x, int y)
{
    if (!g_buf || x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return false;
    return (g_buf[(y / 8) * SCR_W + x] >> (y & 7)) & 1u;
}

struct Ink {
    std::string name;
    int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
    bool empty() const { return x1 < 0; }
    int  h() const { return empty() ? 0 : (y1 - y0 + 1); }
};

/* 画出 _draw 里做的事，然后用"前后缓冲差异"求出这次绘制的墨迹包围盒 */
template <typename F>
static Ink draw_and_measure(const char *name, F _draw)
{
    static uint8_t before[SCR_W * SCR_TILES];
    memcpy(before, g_buf, sizeof(before));

    _draw();

    Ink ink;
    ink.name = name;
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            const uint8_t now  = (g_buf[(y / 8) * SCR_W + x] >> (y & 7)) & 1u;
            const uint8_t was  = (before[(y / 8) * SCR_W + x] >> (y & 7)) & 1u;
            if (now && !was) {
                if (x < ink.x0) ink.x0 = x;
                if (x > ink.x1) ink.x1 = x;
                if (y < ink.y0) ink.y0 = y;
                if (y > ink.y1) ink.y1 = y;
            }
        }
    }
    return ink;
}

static void draw_utf8(float x, float y, const std::string &s)
{
    u8g2_DrawUTF8(&g_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, s.c_str());
}
static void draw_ascii(float x, float y, const std::string &s)
{
    u8g2_DrawStr(&g_u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, s.c_str());
}
static float text_w(const std::string &s)
{
    return (float)u8g2_GetUTF8Width(&g_u8g2, s.c_str());
}

/* ── 主页内容（与 main/astra_glue.cpp 的 refresh_home 一致） ── */
static const char *TITLE     = "智能家居";
static const char *AUTO      = "自动开";
static const char *TEMPHUMI  = "25.3C 52%";
static const char *LIGHTRAIN = "光照 65% 雨滴 6%";
static const char *NET       = "WiFi OK  MQTT OK";

static std::vector<Ink> layout_new(void)
{
    const astra::config &cfg = astra::getUIConfig();
    std::vector<Ink> inks;
    screen_clear();
    u8g2_SetFont(&g_u8g2, cfg.mainFont);
    inks.push_back(draw_and_measure("标题 智能家居(居中)", [&] {
        draw_utf8((SCR_W - text_w(TITLE)) / 2.0f, cfg.rowTitleY, TITLE);
    }));
    inks.push_back(draw_and_measure("右侧 自动开", [&] {
        draw_utf8(SCR_W - 2 - text_w(AUTO), cfg.rowTitleY, AUTO);
    }));
    inks.push_back(draw_and_measure("温湿度 10x20 大字号", [&] {
        u8g2_SetFont(&g_u8g2, u8g2_font_10x20_mn);
        draw_ascii((SCR_W - u8g2_GetStrWidth(&g_u8g2, TEMPHUMI)) / 2.0f, cfg.rowBigY, TEMPHUMI);
        u8g2_SetFont(&g_u8g2, cfg.mainFont);
    }));
    inks.push_back(draw_and_measure("光照/雨滴(居中)", [&] {
        draw_utf8((SCR_W - text_w(LIGHTRAIN)) / 2.0f, cfg.row3Y, LIGHTRAIN);
    }));
    inks.push_back(draw_and_measure("网络状态(居中)", [&] {
        draw_utf8((SCR_W - text_w(NET)) / 2.0f, cfg.row4Y, NET);
    }));
    return inks;
}

/* ── 旧排版（修复前）：y=14 / 38 / 49 / 58 / 62，用来对照 ── */
static std::vector<Ink> layout_old(void)
{
    const astra::config &cfg = astra::getUIConfig();
    std::vector<Ink> inks;
    screen_clear();
    u8g2_SetFont(&g_u8g2, cfg.mainFont);

    const std::string hint = std::string(AUTO) + " OK 菜单";

    inks.push_back(draw_and_measure("标题(旧 y=14)", [&] {
        draw_utf8((SCR_W - text_w(TITLE)) / 2.0f, 14, TITLE);
    }));
    inks.push_back(draw_and_measure("温湿度(旧 y=38)", [&] {
        u8g2_SetFont(&g_u8g2, u8g2_font_10x20_mn);
        draw_ascii((SCR_W - u8g2_GetStrWidth(&g_u8g2, TEMPHUMI)) / 2.0f, 38, TEMPHUMI);
        u8g2_SetFont(&g_u8g2, cfg.mainFont);
    }));
    inks.push_back(draw_and_measure("光照雨滴(旧 y=49)", [&] {
        draw_utf8((SCR_W - text_w(LIGHTRAIN)) / 2.0f, 49, LIGHTRAIN);
    }));
    inks.push_back(draw_and_measure("网络(旧 y=58)", [&] {
        draw_utf8((SCR_W - text_w(NET)) / 2.0f, 58, NET);
    }));
    inks.push_back(draw_and_measure("提示(旧 y=62)", [&] {
        draw_utf8((SCR_W - text_w(hint)) / 2.0f, 62, hint);
    }));
    return inks;
}

static void report(const char *title, const std::vector<Ink> &inks, bool ascii_art)
{
    printf("\n================ %s ================\n", title);
    printf("  %-26s %-16s %-6s %s\n", "元素", "墨迹 x 范围", "行高", "y 范围");
    int overlaps = 0;
    for (size_t i = 0; i < inks.size(); i++) {
        const Ink &k = inks[i];
        char xr[32], yr[32];
        snprintf(xr, sizeof(xr), "%3d..%3d", k.x0, k.x1);
        snprintf(yr, sizeof(yr), "%2d..%2d", k.y0, k.y1);
        printf("  %-26s %-16s %4dpx  %s", k.name.c_str(), k.empty() ? "(空)" : xr, k.h(), yr);
        if (i > 0) {
            const Ink &p = inks[i - 1];
            /* 真正的"看不清"= y 范围重叠【且】x 范围也重叠（同一行的左右两段不算冲突） */
            const bool y_ov = (!k.empty() && !p.empty() && k.y0 <= p.y1);
            const bool x_ov = (!k.empty() && !p.empty() && !(k.x1 < p.x0 || k.x0 > p.x1));
            if (y_ov && x_ov) {
                overlaps++;
                printf("   ← ★与上一行重叠 %dpx", p.y1 - k.y0 + 1);
            } else if (y_ov) {
                printf("   （同一行，左右两段）");
            } else if (!k.empty() && !p.empty()) {
                printf("   行间距 %dpx", k.y0 - p.y1 - 1);
            }
        }
        printf("\n");
    }
    printf("  → 真正重叠：%d 处\n", overlaps);

    /* 行盒占用图：一眼看出哪几行挤在一起 */
    printf("\n  ---- 纵向占用（每格 1px，0..63）----\n");
    for (size_t i = 0; i < inks.size(); i++) {
        const Ink &k = inks[i];
        if (k.empty()) continue;
        printf("  %-24s |", k.name.c_str());
        for (int y = 0; y < SCR_H; y++) putchar((y >= k.y0 && y <= k.y1) ? '#' : '.');
        printf("|\n");
    }
    if (ascii_art) {
        printf("\n  ---- 渲染结果（128x64，'#'=点亮）----\n");
        printf("     +");
        for (int x = 0; x < SCR_W; x++) putchar('-');
        printf("+\n");
        for (int y = 0; y < SCR_H; y++) {
            printf("  %2d |", y);
            for (int x = 0; x < SCR_W; x++) putchar(px(x, y) ? '#' : ' ');
            printf("|\n");
        }
        printf("     +");
        for (int x = 0; x < SCR_W; x++) putchar('-');
        printf("+\n");
    }
}

int main(void)
{
    /* 标准 SSD1306 128x64 全缓冲 setup（回调是空的：只画进 RAM，不发 I2C）。
     * ★ 必须走这个 setup：它同时建立 u8x8.display_info 并分配内部缓冲；
     *   直接调 u8g2_SetupBuffer 会因为 display_info 为空而在绘制时崩（0xC0000005）。 */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&g_u8g2, U8G2_R0, u8x8_dummy_msg, u8x8_dummy_msg);
    g_buf = g_u8g2.tile_buf_ptr;
    if (g_buf == nullptr) { printf("u8g2 setup failed\n"); return 1; }

    u8g2_SetFont(&g_u8g2, u8g2_font_wqy12_t_gb2312);
    const int h_cn = u8g2_GetMaxCharHeight(&g_u8g2);
    u8g2_SetFont(&g_u8g2, u8g2_font_10x20_mn);
    const int h_big = u8g2_GetMaxCharHeight(&g_u8g2);

    printf("128x64 排版预览（字体：wqy12 中文 行高 %dpx，10x20_mn 大字号 行高 %dpx）\n",
           h_cn, h_big);

    report("修复前（旧坐标 y=14/38/49/58/62）", layout_old(), false);
    report("修复后（config.h 行基线 11/29/45/58）", layout_new(), true);

    return 0;
}
