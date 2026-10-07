#pragma once
#ifndef ASTRA_CORE_SRC_HAL_HAL_H_
#define ASTRA_CORE_SRC_HAL_HAL_H_

#include <string>
#include <utility>
#include <vector>
#include "../astra/config/config.h"

namespace oled {

}

namespace key {
typedef enum keyAction {
  RELEASE = 0,
  CLICK,
  PRESS,
} KEY_ACTION;

typedef enum keyIndex {
  KEY_UP = 0,
  KEY_DOWN,
  KEY_LEFT,
  KEY_RIGHT,
  KEY_OK,
  KEY_NUM,
} KEY_INDEX;
}

namespace buzzer {

}

namespace led {

}

namespace sys {
struct config {
  uint8_t screenWeight = 128;
  uint8_t screenHeight = 64;
  float screenBright = 255;
  // 系统参数先这些
};

static config &getSystemConfig() {
  static config sysConfig;
  return sysConfig;
}
}

// 硬件接口总表
class HAL {
private:
  static HAL *hal;

public:
  // 拿硬件实例
  static HAL *get();
  // 查有没有实例
  static bool check();

  // 装上实例并开跑
  static bool inject(HAL *_hal);
  // 把实例拆掉
  static void destroy();

  virtual ~HAL() = default;

  virtual std::string type() { return "Base"; }

  virtual void init() {}

protected:
  sys::config config;

public:
  static void *getCanvasBuffer() { return get()->_getCanvasBuffer(); }

  virtual void *_getCanvasBuffer() { return nullptr; }

  static uint8_t getBufferTileHeight() { return get()->_getBufferTileHeight(); }

  virtual uint8_t _getBufferTileHeight() { return 0; }

  static uint8_t getBufferTileWidth() { return get()->_getBufferTileWidth(); }

  virtual uint8_t _getBufferTileWidth() { return 0; }

  static void canvasUpdate() { get()->_canvasUpdate(); }

  virtual void _canvasUpdate() {}

  static void canvasClear() { get()->_canvasClear(); }

  virtual void _canvasClear() {}

  static void setFont(const uint8_t *_font) { get()->_setFont(_font); }

  virtual void _setFont(const uint8_t *_font) {}

  static uint8_t getFontWidth(std::string &_text) { return get()->_getFontWidth(_text); }

  virtual uint8_t _getFontWidth(std::string &_text) { return 0; }

  // 量长文用 16 位宽
  static uint16_t getFontWidthU16(std::string &_text) { return get()->_getFontWidthU16(_text); }

  virtual uint16_t _getFontWidthU16(std::string &_text) { return 0; }

  static uint8_t getFontHeight() { return get()->_getFontHeight(); }

  virtual uint8_t _getFontHeight() { return 0; }

  static void setDrawType(uint8_t _type) { get()->_setDrawType(_type); }

  virtual void _setDrawType(uint8_t _type) {}

  static void drawPixel(float _x, float _y) { get()->_drawPixel(_x, _y); }

  virtual void _drawPixel(float _x, float _y) {}

  // 画字的坐标是左下角
  static void drawEnglish(float _x, float _y, const std::string &_text) { get()->_drawEnglish(_x, _y, _text); }

  virtual void _drawEnglish(float _x, float _y, const std::string &_text) {}

  static void drawChinese(float _x, float _y, const std::string &_text) { get()->_drawChinese(_x, _y, _text); }

  virtual void _drawChinese(float _x, float _y, const std::string &_text) {}

  static void drawVDottedLine(float _x, float _y, float _h) { get()->_drawVDottedLine(_x, _y, _h); }

  virtual void _drawVDottedLine(float _x, float _y, float _h) {}

  static void drawHDottedLine(float _x, float _y, float _l) { get()->_drawHDottedLine(_x, _y, _l); }

  virtual void _drawHDottedLine(float _x, float _y, float _l) {}

  static void drawVLine(float _x, float _y, float _h) { get()->_drawVLine(_x, _y, _h); }

  virtual void _drawVLine(float _x, float _y, float _h) {}

  static void drawHLine(float _x, float _y, float _l) { get()->_drawHLine(_x, _y, _l); }

  virtual void _drawHLine(float _x, float _y, float _l) {}

  static void drawBMP(float _x, float _y, float _w, float _h, const uint8_t *_bitMap) {
    get()->_drawBMP(_x,
                    _y,
                    _w,
                    _h,
                    _bitMap);
  }

  virtual void _drawBMP(float _x, float _y, float _w, float _h, const uint8_t *_bitMap) {}

  static void drawBox(float _x, float _y, float _w, float _h) { get()->_drawBox(_x, _y, _w, _h); }

  virtual void _drawBox(float _x, float _y, float _w, float _h) {}

  static void drawRBox(float _x, float _y, float _w, float _h, float _r) {
    get()->_drawRBox(_x,
                     _y,
                     _w,
                     _h,
                     _r);
  }

  virtual void _drawRBox(float _x, float _y, float _w, float _h, float _r) {}

  static void drawFrame(float _x, float _y, float _w, float _h) { get()->_drawFrame(_x, _y, _w, _h); }

  virtual void _drawFrame(float _x, float _y, float _w, float _h) {}

  static void drawRFrame(float _x, float _y, float _w, float _h, float _r) {
    get()->_drawRFrame(_x,
                       _y,
                       _w,
                       _h,
                       _r);
  }

  virtual void _drawRFrame(float _x, float _y, float _w, float _h, float _r) {}

  static void printInfo(std::string _msg) { get()->_printInfo(std::move(_msg)); }

  virtual void _printInfo(std::string _msg);

  // 系统时间接口
public:
  static void delay(unsigned long _mill) { get()->_delay(_mill); }

  virtual void _delay(unsigned long _mill) {}

  static unsigned long millis() { return get()->_millis(); }

  virtual unsigned long _millis() { return 0; }

  static unsigned long getTick() { return get()->_getTick(); }

  virtual unsigned long _getTick() { return 0; }

  static unsigned long getRandomSeed() { return get()->_getRandomSeed(); }

  // 这个可以不实现
  virtual unsigned long _getRandomSeed() { return 0; }

  // 蜂鸣器接口
public:
  static void beep(float _freq) { get()->_beep(_freq); }

  virtual void _beep(float _freq) {}

  static void beepStop() { get()->_beepStop(); }

  virtual void _beepStop() {}

  static void setBeepVol(uint8_t _vol) { get()->_setBeepVol(_vol); }

  virtual void _setBeepVol(uint8_t _vol) {}

  static void screenOn() { get()->_screenOn(); }

  virtual void _screenOn() {}

  static void screenOff() { get()->_screenOff(); }

  virtual void _screenOff() {}

  // 按键接口
public:
  static bool getKey(key::KEY_INDEX _keyIndex) { return get()->_getKey(_keyIndex); }

  virtual bool _getKey(key::KEY_INDEX _keyIndex) { return false; }

  static bool getAnyKey() { return get()->_getAnyKey(); }

  virtual bool _getAnyKey();

  // 读按键当前动作
  static key::keyAction getKeyAction(key::KEY_INDEX _keyIndex) { return get()->key[_keyIndex]; }

  // 清掉按键动作
  static void clearKeyActions() {
    for (int i = 0; i < key::KEY_NUM; i++) get()->key[i] = key::RELEASE;
  }

protected:
  key::keyAction key[key::KEY_NUM] = {static_cast<key::keyAction>(0)};

public:
  static void keyScan() { get()->_keyScan(); }

  virtual void _keyScan();

  static void keyTest() { return get()->_keyTest(); }

  virtual void _keyTest();

  // 系统参数接口
public:
  static sys::config &getSystemConfig() { return get()->config; }

  static void setSystemConfig(sys::config _cfg) { get()->config = _cfg; }

  static void updateConfig() { get()->_updateConfig(); }

  virtual void _updateConfig() {}
};

#endif
