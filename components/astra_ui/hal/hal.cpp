//
// Created by Fir on 2024/2/8.
//

#include <cstring>
#include "hal.h"

HAL *HAL::hal = nullptr;

HAL *HAL::get() {
  return hal;
}

bool HAL::check() {
  return hal != nullptr;
}

bool HAL::inject(HAL *_hal) {
  if (_hal == nullptr) {
    return false;
  }

  _hal->init();
  hal = _hal;
  return true;
}

void HAL::destroy() {
  if (hal == nullptr) return;

  delete hal;
  hal = nullptr;
}

/**
 * @brief log printer. 自动换行的日志输出
 *
 * @param _msg message want to print. 要输出的信息
 * @note cannot execute within a loop. 不能在循环内执行
 */
void HAL::_printInfo(std::string _msg) {
  static std::vector<std::string> _infoCache = {};
  static const uint8_t _max = getSystemConfig().screenHeight / getFontHeight();
  static const uint8_t _fontHeight = getFontHeight();

  if (_infoCache.size() >= _max) _infoCache.clear();
  _infoCache.push_back(_msg);

  canvasClear();
  setDrawType(2); //反色显示
  for (uint8_t i = 0; i < _infoCache.size(); i++) {
    drawEnglish(0, _fontHeight + i * (1 + _fontHeight), _infoCache[i]);
  }
  canvasUpdate();
  setDrawType(1); //回归实色显示
}

bool HAL::_getAnyKey() {
  for (int i = 0; i < key::KEY_NUM; i++) {
    if (getKey(static_cast<key::KEY_INDEX>(i))) return true;
  }
  return false;
}

/**
 * @brief key scanner default. 默认按键扫描函数（通用 KEY_NUM 键）
 *
 * 状态机：空闲检测新按下 → 确认键跟踪（时间基准长按 1s → PRESS）→ 释放（短按 → CLICK）。
 * 时间基准（millis）而非扫描次数，帧率无关。
 */
void HAL::_keyScan() {
  static int8_t _confirmedKey = -1;   /* 正在跟踪的键索引 */
  static bool _waitRelease = false;   /* 长按触发后等待释放（避免重复 PRESS） */
  static unsigned long _pressAt = 0;

  if (_waitRelease) {
    if (!getAnyKey()) _waitRelease = false;
    return;
  }

  if (_confirmedKey < 0) {
    /* 空闲：检测新按下（防抖：连续两次扫描均按下才确认） */
    for (int i = 0; i < key::KEY_NUM; i++) {
      if (getKey(static_cast<key::KEY_INDEX>(i))) {
        _confirmedKey = i;
        _pressAt = millis();
        break;
      }
    }
  } else {
    if (getKey(static_cast<key::KEY_INDEX>(_confirmedKey))) {
      if (millis() - _pressAt > 1000) {       /* 长按 1s → PRESS */
        key[_confirmedKey] = key::PRESS;
        _confirmedKey = -1;
        _waitRelease = true;
      }
    } else {
      key[_confirmedKey] = key::CLICK;        /* 释放：短按 → CLICK */
      _confirmedKey = -1;
    }
  }
}

/**
 * @brief default key tester. 默认按键测试函数
 */
void HAL::_keyTest() {
  if (getAnyKey()) {
    for (uint8_t i = 0; i < key::KEY_NUM; i++) {
      if (key[i] == key::CLICK) {
        //do something when key clicked
        break;
      } else if (key[i] == key::PRESS) {
        //do something when key pressed
        break;
      }
    }
    clearKeyActions();
  }
}
