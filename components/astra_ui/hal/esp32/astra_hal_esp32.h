//
// astra_hal_esp32.h
// ESP32 移植的 astra HAL：u8g2 全缓冲纯软件画布（不发送显示），
// 刷新经注入的回调交给 main 组件的 OLED 驱动（每页两事务实证模式）。
//
#pragma once
#ifndef ASTRA_HAL_ESP32_H_
#define ASTRA_HAL_ESP32_H_

#include "../hal.h"

/**
 * @brief main 组件注入的桥接回调（ESP-IDF 组件不能反向依赖 main 组件）。
 * @param flush_page 按页刷新：data = 128 字节页数据（SH1106 页格式）。
 *                   HAL 内部做脏页跟踪，仅变化页会触发回调 → 动画更丝滑。
 * @param key_down   某键当前是否按下（瞬时状态，供 astra keyScan 状态机使用）
 * @param beep       蜂鸣器发声（频率 Hz）
 */
void astra_hal_set_flush_page_cb(void (*flush_page)(uint8_t page, const uint8_t *data));
void astra_hal_set_key_down_cb(bool (*key_down)(uint8_t idx));
void astra_hal_set_beep_cb(void (*beep)(float freq));

/** ESP32 版 HAL 实现 */
class AstraHALEsp32 : public HAL {
public:
  ~AstraHALEsp32() override = default;

  std::string type() override { return "ESP32"; }

  void init() override;

protected:
  void *_getCanvasBuffer() override;
  uint8_t _getBufferTileHeight() override;
  uint8_t _getBufferTileWidth() override;
  void _canvasUpdate() override;
  void _canvasClear() override;

  void _setFont(const uint8_t *_font) override;
  uint8_t _getFontWidth(std::string &_text) override;
  uint16_t _getFontWidthU16(std::string &_text) override;
  uint8_t _getFontHeight() override;

  void _setDrawType(uint8_t _type) override;

  void _drawPixel(float _x, float _y) override;
  void _drawEnglish(float _x, float _y, const std::string &_text) override;
  void _drawChinese(float _x, float _y, const std::string &_text) override;
  void _drawVDottedLine(float _x, float _y, float _h) override;
  void _drawHDottedLine(float _x, float _y, float _l) override;
  void _drawVLine(float _x, float _y, float _h) override;
  void _drawHLine(float _x, float _y, float _l) override;
  void _drawBMP(float _x, float _y, float _w, float _h, const uint8_t *_bitMap) override;
  void _drawBox(float _x, float _y, float _w, float _h) override;
  void _drawRBox(float _x, float _y, float _w, float _h, float _r) override;
  void _drawFrame(float _x, float _y, float _w, float _h) override;
  void _drawRFrame(float _x, float _y, float _w, float _h, float _r) override;

  void _delay(unsigned long _mill) override;
  unsigned long _millis() override;
  unsigned long _getTick() override;
  unsigned long _getRandomSeed() override;

  void _beep(float _freq) override;
  void _beepStop() override;

  bool _getKey(key::KEY_INDEX _keyIndex) override;
};

#endif //ASTRA_HAL_ESP32_H_
