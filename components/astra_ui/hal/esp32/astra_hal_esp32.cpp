//
// astra_hal_esp32.cpp
// ESP32 移植的 astra HAL 实现。
//
#include <cstring>
#include "astra_hal_esp32.h"
#include "u8g2.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---------- main 组件注入的桥接回调 ---------- */
static void (*g_flush_page_cb)(uint8_t page, const uint8_t *data) = nullptr;
static bool (*g_key_down_cb)(uint8_t idx) = nullptr;
static void (*g_beep_cb)(float freq) = nullptr;

void astra_hal_set_flush_page_cb(void (*flush_page)(uint8_t page, const uint8_t *data)) { g_flush_page_cb = flush_page; }
void astra_hal_set_key_down_cb(bool (*key_down)(uint8_t idx)) { g_key_down_cb = key_down; }
void astra_hal_set_beep_cb(void (*beep)(float freq)) { g_beep_cb = beep; }

/* ---------- 纯软件画布：u8g2 全缓冲实例，绝不发送显示 ---------- */

static u8g2_t g_canvas;

static uint8_t dummy_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 0;   /* 纯软件画布：字节过程不接任何硬件 */
}

static uint8_t dummy_gpio_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 0;
}

void AstraHALEsp32::init()
{
    /* 全缓冲软件画布（SH1106 128×64 同布局：16×8 瓦片，竖列 LSB=页字节格式） */
    u8g2_Setup_ssd1306_128x64_noname_f(&g_canvas, U8G2_R0, dummy_byte_cb, dummy_gpio_cb);   /* 0.96" SSD1306(原项目 1.3" SH1106)*/
    u8g2_SetFont(&g_canvas, astra::getUIConfig().mainFont);
}

void *AstraHALEsp32::_getCanvasBuffer()
{
    return u8g2_GetBufferPtr(&g_canvas);
}

uint8_t AstraHALEsp32::_getBufferTileHeight()
{
    return 8;
}

uint8_t AstraHALEsp32::_getBufferTileWidth()
{
    return 16;
}

void AstraHALEsp32::_canvasClear()
{
    u8g2_ClearBuffer(&g_canvas);
}

void AstraHALEsp32::_canvasUpdate()
{
    /* u8g2 全缓冲为页连续布局（u8g2_ll_hvline.c: offset=(y&~7)*16+x），
     * 第 p 页 = buf[p*128..p*128+127]，字节竖列 LSB 在顶，与 SH1106 页格式一致。
     * 脏页跟踪：只写变化页。静态界面零 I²C 流量，选择框/文字等局部动画
     * 只写 1~5 页（每页 ~6ms），动画帧率大幅提升。 */
    static uint8_t last[8 * 128];
    static bool inited = false;

    const uint8_t *buf = u8g2_GetBufferPtr(&g_canvas);
    if (g_flush_page_cb != nullptr) {
        for (int p = 0; p < 8; p++) {
            const uint8_t *src = buf + p * 128;
            if (!inited || memcmp(src, &last[p * 128], 128) != 0) {
                g_flush_page_cb((uint8_t)p, src);
                memcpy(&last[p * 128], src, 128);
            }
        }
    } else {
        memcpy(last, buf, sizeof(last));
    }
    inited = true;

    /* 让出 CPU 喂任务看门狗：框架的阻塞渲染循环（弹窗/校准）内部没有延时。
     * 必须 vTaskDelay(1)（1 tick）——pdMS_TO_TICKS(1) 在 100Hz 下整数除法=0。
     * 本机 CONFIG_FREERTOS_HZ=1000 → 1 tick = 1ms。 */
    vTaskDelay(1);
}

/* ---------- 字体 ---------- */

void AstraHALEsp32::_setFont(const uint8_t *_font)
{
    u8g2_SetFont(&g_canvas, _font);
}

uint8_t AstraHALEsp32::_getFontWidth(std::string &_text)
{
    return (uint8_t)u8g2_GetUTF8Width(&g_canvas, _text.c_str());
}

uint16_t AstraHALEsp32::_getFontWidthU16(std::string &_text)
{
    return (uint16_t)u8g2_GetUTF8Width(&g_canvas, _text.c_str());
}

uint8_t AstraHALEsp32::_getFontHeight()
{
    return (uint8_t)u8g2_GetMaxCharHeight(&g_canvas);
}

void AstraHALEsp32::_setDrawType(uint8_t _type)
{
    u8g2_SetDrawColor(&g_canvas, _type);    /* astra: 0=关 1=实色 2=反色(XOR) 与 u8g2 一致 */
}

/* ---------- 绘制原语（坐标约定：文字为左下角/基线，几何为左上角，与框架一致） ---------- */

void AstraHALEsp32::_drawPixel(float _x, float _y)
{
    u8g2_DrawPixel(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y);
}

void AstraHALEsp32::_drawEnglish(float _x, float _y, const std::string &_text)
{
    u8g2_DrawStr(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, _text.c_str());
}

void AstraHALEsp32::_drawChinese(float _x, float _y, const std::string &_text)
{
    u8g2_DrawUTF8(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, _text.c_str());
}

void AstraHALEsp32::_drawVDottedLine(float _x, float _y, float _h)
{
    for (float y = _y; y < _y + _h; y += 2) u8g2_DrawPixel(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)y);
}

void AstraHALEsp32::_drawHDottedLine(float _x, float _y, float _l)
{
    for (float x = _x; x < _x + _l; x += 2) u8g2_DrawPixel(&g_canvas, (u8g2_uint_t)x, (u8g2_uint_t)_y);
}

void AstraHALEsp32::_drawVLine(float _x, float _y, float _h)
{
    u8g2_DrawVLine(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, (u8g2_uint_t)_h);
}

void AstraHALEsp32::_drawHLine(float _x, float _y, float _l)
{
    u8g2_DrawHLine(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, (u8g2_uint_t)_l);
}

void AstraHALEsp32::_drawBMP(float _x, float _y, float _w, float _h, const uint8_t *_bitMap)
{
    u8g2_DrawXBMP(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y,
                  (u8g2_uint_t)_w, (u8g2_uint_t)_h, _bitMap);
}

void AstraHALEsp32::_drawBox(float _x, float _y, float _w, float _h)
{
    u8g2_DrawBox(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, (u8g2_uint_t)_w, (u8g2_uint_t)_h);
}

void AstraHALEsp32::_drawRBox(float _x, float _y, float _w, float _h, float _r)
{
    u8g2_DrawRBox(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y,
                  (u8g2_uint_t)_w, (u8g2_uint_t)_h, (u8g2_uint_t)_r);
}

void AstraHALEsp32::_drawFrame(float _x, float _y, float _w, float _h)
{
    u8g2_DrawFrame(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, (u8g2_uint_t)_w, (u8g2_uint_t)_h);
}

void AstraHALEsp32::_drawRFrame(float _x, float _y, float _w, float _h, float _r)
{
    u8g2_DrawRFrame(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y,
                    (u8g2_uint_t)_w, (u8g2_uint_t)_h, (u8g2_uint_t)_r);
}

/* ---------- 系统 ---------- */

void AstraHALEsp32::_delay(unsigned long _mill)
{
    vTaskDelay(pdMS_TO_TICKS(_mill));
}

unsigned long AstraHALEsp32::_millis()
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

unsigned long AstraHALEsp32::_getTick()
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

unsigned long AstraHALEsp32::_getRandomSeed()
{
    return (unsigned long)esp_random();
}

/* ---------- 蜂鸣器 ---------- */

void AstraHALEsp32::_beep(float _freq)
{
    if (g_beep_cb != nullptr) g_beep_cb(_freq);
}

void AstraHALEsp32::_beepStop()
{
    /* 本机蜂鸣器为短促阻塞发声，无需停止 */
}

/* ---------- 按键 ---------- */

bool AstraHALEsp32::_getKey(key::KEY_INDEX _keyIndex)
{
    if (g_key_down_cb == nullptr) return false;
    return g_key_down_cb((uint8_t)_keyIndex);
}
