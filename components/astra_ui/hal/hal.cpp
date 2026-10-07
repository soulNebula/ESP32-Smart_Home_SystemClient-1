#include <cstring>
#include "hal.h"

HAL *HAL::hal = nullptr;

// 拿硬件实例
HAL *HAL::get() {
  return hal;
}

// 查有没有实例
bool HAL::check() {
  return hal != nullptr;
}

// 装上实例并开跑
bool HAL::inject(HAL *_hal) {
  if (_hal == nullptr) {
    return false;
  }

  _hal->init();
  hal = _hal;
  return true;
}

// 把实例拆掉
void HAL::destroy() {
  if (hal == nullptr) return;

  delete hal;
  hal = nullptr;
}

// 把日志打到屏上
void HAL::_printInfo(std::string _msg) {
  static std::vector<std::string> _infoCache = {};
  static const uint8_t _max = getSystemConfig().screenHeight / getFontHeight();
  static const uint8_t _fontHeight = getFontHeight();

  if (_infoCache.size() >= _max) _infoCache.clear();
  _infoCache.push_back(_msg);

  canvasClear();
  // 反色显示
  setDrawType(2);
  for (uint8_t i = 0; i < _infoCache.size(); i++) {
    drawEnglish(0, _fontHeight + i * (1 + _fontHeight), _infoCache[i]);
  }
  canvasUpdate();
  // 回到实色
  setDrawType(1);
}

// 看有没有键按下
bool HAL::_getAnyKey() {
  for (int i = 0; i < key::KEY_NUM; i++) {
    if (getKey(static_cast<key::KEY_INDEX>(i))) return true;
  }
  return false;
}

// 扫按键分长短按
void HAL::_keyScan() {
  // 正在盯的键
  static int8_t _confirmedKey = -1;
  // 等松手防重复
  static bool _waitRelease = false;
  static unsigned long _pressAt = 0;

  if (_waitRelease) {
    if (!getAnyKey()) _waitRelease = false;
    return;
  }

  if (_confirmedKey < 0) {
    // 空闲时找新按下
    for (int i = 0; i < key::KEY_NUM; i++) {
      if (getKey(static_cast<key::KEY_INDEX>(i))) {
        _confirmedKey = i;
        _pressAt = millis();
        break;
      }
    }
  } else {
    if (getKey(static_cast<key::KEY_INDEX>(_confirmedKey))) {
      // 按住一秒算长按
      if (millis() - _pressAt > 1000) {
        key[_confirmedKey] = key::PRESS;
        _confirmedKey = -1;
        _waitRelease = true;
      }
    } else {
      // 松手算短按
      key[_confirmedKey] = key::CLICK;
      _confirmedKey = -1;
    }
  }
}

// 按键测试留空壳
void HAL::_keyTest() {
  if (getAnyKey()) {
    for (uint8_t i = 0; i < key::KEY_NUM; i++) {
      if (key[i] == key::CLICK) {
        // 短按想干啥写这
        break;
      } else if (key[i] == key::PRESS) {
        // 长按想干啥写这
        break;
      }
    }
    clearKeyActions();
  }
}
