/*
 * 模块：
 *   硬件接口的公共部分。管那唯一一个硬件实例的装、取、拆，还写了
 *   按键扫描和打日志的默认做法，被界面层调，具体硬件在 esp32 目录下。
 *
 * 功能：
 *   装拆硬件实例
 *   扫按键分长短按
 *   把日志打到屏上
 */

#include <cstring>
#include "hal.h"

HAL *HAL::hal = nullptr;

/* 功能：拿硬件实例 */
HAL *HAL::get() {
  return hal;
}

/* 功能：查有没有实例 */
bool HAL::check() {
  return hal != nullptr;
}

/* 功能：装上实例并开跑 */
bool HAL::inject(HAL *_hal) {
  if (_hal == nullptr) {
    return false;
  }

  _hal->init();
  hal = _hal;
  return true;
}

/* 功能：把实例拆掉 */
void HAL::destroy() {
  if (hal == nullptr) return;

  delete hal;
  hal = nullptr;
}

/* 功能：把日志打到屏上 */
void HAL::_printInfo(std::string _msg) {
  static std::vector<std::string> _infoCache = {};
  static const uint8_t _max = getSystemConfig().screenHeight / getFontHeight();
  static const uint8_t _fontHeight = getFontHeight();

  if (_infoCache.size() >= _max) _infoCache.clear();
  _infoCache.push_back(_msg);

  canvasClear();
  setDrawType(2); /* 功能：反色显示 */
  for (uint8_t i = 0; i < _infoCache.size(); i++) {
    drawEnglish(0, _fontHeight + i * (1 + _fontHeight), _infoCache[i]);
  }
  canvasUpdate();
  setDrawType(1); /* 功能：回到实色 */
}

/* 功能：看有没有键按下 */
bool HAL::_getAnyKey() {
  for (int i = 0; i < key::KEY_NUM; i++) {
    if (getKey(static_cast<key::KEY_INDEX>(i))) return true;
  }
  return false;
}

/* 功能：扫按键分长短按 */
void HAL::_keyScan() {
  static int8_t _confirmedKey = -1;   /* 功能：正在盯的键 */
  static bool _waitRelease = false;   /* 功能：等松手防重复 */
  static unsigned long _pressAt = 0;

  if (_waitRelease) {
    if (!getAnyKey()) _waitRelease = false;
    return;
  }

  if (_confirmedKey < 0) {
    /* 功能：空闲时找新按下 */
    for (int i = 0; i < key::KEY_NUM; i++) {
      if (getKey(static_cast<key::KEY_INDEX>(i))) {
        _confirmedKey = i;
        _pressAt = millis();
        break;
      }
    }
  } else {
    if (getKey(static_cast<key::KEY_INDEX>(_confirmedKey))) {
      if (millis() - _pressAt > 1000) {       /* 功能：按住一秒算长按 */
        key[_confirmedKey] = key::PRESS;
        _confirmedKey = -1;
        _waitRelease = true;
      }
    } else {
      key[_confirmedKey] = key::CLICK;        /* 功能：松手算短按 */
      _confirmedKey = -1;
    }
  }
}

/* 功能：按键测试留空壳 */
void HAL::_keyTest() {
  if (getAnyKey()) {
    for (uint8_t i = 0; i < key::KEY_NUM; i++) {
      if (key[i] == key::CLICK) {
        /* 功能：短按想干啥写这 */
        break;
      } else if (key[i] == key::PRESS) {
        /* 功能：长按想干啥写这 */
        break;
      }
    }
    clearKeyActions();
  }
}
