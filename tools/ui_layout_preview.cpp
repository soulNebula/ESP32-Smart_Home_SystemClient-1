/*
 * 模块：
 *   屏幕排版的电脑预览。不接板子不烧录，把首页要显示的字画到内存里，
 *   算一算每个元素的占位范围，打印一张字符画和重叠报告。
 *   本工程的屏幕只有 64 像素高，中文行高大，凭手感给坐标很容易把两行叠一起，
 *   所以改排版前先在这儿看一眼。用的是和固件同一份配置和同一套字库，
 *   由 tools/run_ui_preview.ps1 编译运行，排版参数在 astra_ui 的 config.h。
 *   判定重叠要上下和左右两个方向都压住才算，只压住一边只是同一行的两段。
 *   屏幕初始化那一步必须走：它会顺手建好屏幕信息和内存，缺了画的时候会崩。
 *
 * 功能：
 *   画一遍新排版
 *   画一遍旧排版
 *   报告有没有重叠
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "u8g2.h"          /* 功能：字库头文件 */
}

/* 功能：共用固件的排版参数 */
#include "config.h"

#define SCR_W 128
#define SCR_H 64
#define SCR_TILES (SCR_H / 8)

static u8g2_t  g_u8g2;
static uint8_t *g_buf = nullptr;   /* 功能：指向屏幕内存 */

/* 功能：空回调，只画不发送 */
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

/* 功能：画完对比前后找范围 */
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

/* 功能：首页要显示的内容 */
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

/* 功能：修改前的老坐标，对照用 */
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
            /* 功能：上下左右都压住才算 */
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

    /* 功能：看哪几行挤在一起 */
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
    /* 功能：必须走这个初始化 */
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
