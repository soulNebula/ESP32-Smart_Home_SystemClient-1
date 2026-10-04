/*
 * 模块：
 *   界面层的画布实现。在内存里用 u8g2 画图，画完只把变了的那几页
 *   交给 main 的屏幕驱动送出去；按键和蜂鸣器走 main 送来的口，
 *   被 astra_rocket.cpp 装上使用。
 *
 * 功能：
 *   开一块画布
 *   只刷变化页
 *   画字画框画点
 */
#include <cstring>
#include "astra_hal_esp32.h"
#include "u8g2.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 功能：main 送来的三个口 */
static void (*g_flush_page_cb)(uint8_t page, const uint8_t *data) = nullptr;
static bool (*g_key_down_cb)(uint8_t idx) = nullptr;
static void (*g_beep_cb)(float freq) = nullptr;

void astra_hal_set_flush_page_cb(void (*flush_page)(uint8_t page, const uint8_t *data)) { g_flush_page_cb = flush_page; }
void astra_hal_set_key_down_cb(bool (*key_down)(uint8_t idx)) { g_key_down_cb = key_down; }
void astra_hal_set_beep_cb(void (*beep)(float freq)) { g_beep_cb = beep; }

/* 功能：内存里的整屏画布 */

static u8g2_t g_canvas;

static uint8_t dummy_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 0;   /* 功能：不接任何硬件 */
}

static uint8_t dummy_gpio_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)msg; (void)arg_int; (void)arg_ptr;
    return 0;
}

/* 功能：把画布准备好 */
void AstraHALEsp32::init()
{
    /* 功能：开一块整屏画布 */
    u8g2_Setup_ssd1306_128x64_noname_f(&g_canvas, U8G2_R0, dummy_byte_cb, dummy_gpio_cb);   /* 功能：用屏幕芯片的驱动 */
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

/* 功能：把画布擦干净 */
void AstraHALEsp32::_canvasClear()
{
    u8g2_ClearBuffer(&g_canvas);
}

/* 功能：把变了的页送出去 */
void AstraHALEsp32::_canvasUpdate()
{
    /* 功能：只把变了的页送出去 */
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

    /* 功能：让一下 CPU 喂狗 */
    vTaskDelay(1);
}

/* 功能：字体相关 */

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
    u8g2_SetDrawColor(&g_canvas, _type);    /* 功能：0关 1实 2反色 */
}

/* 功能：画点画线画字 */

void AstraHALEsp32::_drawPixel(float _x, float _y)
{
    u8g2_DrawPixel(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y);
}

/* 功能：画英文字 */
void AstraHALEsp32::_drawEnglish(float _x, float _y, const std::string &_text)
{
    u8g2_DrawStr(&g_canvas, (u8g2_uint_t)_x, (u8g2_uint_t)_y, _text.c_str());
}

/* 功能：画中文字 */
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

/* 功能：时间相关 */

/* 功能：等一会儿 */
void AstraHALEsp32::_delay(unsigned long _mill)
{
    vTaskDelay(pdMS_TO_TICKS(_mill));
}

/* 功能：开机到现在几毫秒 */
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

/* 功能：蜂鸣器相关 */

/* 功能：响一声 */
void AstraHALEsp32::_beep(float _freq)
{
    if (g_beep_cb != nullptr) g_beep_cb(_freq);
}

/* 功能：本机不用停 */
void AstraHALEsp32::_beepStop()
{
    /* 功能：响了就自己停 */
}

/* 功能：按键相关 */

/* 功能：问某键按下没 */
bool AstraHALEsp32::_getKey(key::KEY_INDEX _keyIndex)
{
    if (g_key_down_cb == nullptr) return false;
    return g_key_down_cb((uint8_t)_keyIndex);
}
