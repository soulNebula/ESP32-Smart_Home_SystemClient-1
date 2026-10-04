/*
 * 模块：
 *   界面的零件库。页面、选中框、画面镜头这些零件都在这儿，被
 *   launcher.cpp、item.cpp 和 astra_rocket.cpp 用，向下调 hal 画屏。
 *
 * 功能：
 *   组页面和零件
 *   算每行的位置
 *   做动画和模糊
 */

#pragma once
#ifndef ASTRA_ASTRA__H
#define ASTRA_ASTRA__H

#include "cstdint"
#include "string"
#include <vector>
#include "../../../../hal/hal.h"
#include "../../../../astra/config/config.h"
#include <cmath>

namespace astra {

/* 功能：零件基类 */
class Item {
protected:
  sys::config systemConfig;
  config astraConfig;

  void updateConfig();
};

/* 功能：动画基类 */
class Animation {
public:
  virtual void entryAnimation();
  virtual void exitAnimation();
  virtual void blur();
  virtual void animation(float *_pos, float _posTrg, float _speed);
};

inline void Animation::entryAnimation() { }

/* 功能：退场时闪一下屏 */
inline void Animation::exitAnimation() {
  static uint8_t fadeFlag = 1;
  static uint8_t bufferLen = 8 * HAL::getBufferTileHeight() * HAL::getBufferTileWidth();
  auto *bufferPointer = (uint8_t *) HAL::getCanvasBuffer();

  HAL::delay(getUIConfig().fadeAnimationSpeed);

  if (getUIConfig().lightMode)
    switch (fadeFlag) {
      case 1:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 != 0) bufferPointer[i] = bufferPointer[i] & 0xAA;
        break;
      case 2:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 != 0) bufferPointer[i] = bufferPointer[i] & 0x00;
        break;
      case 3:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 == 0) bufferPointer[i] = bufferPointer[i] & 0x55;
        break;
      case 4:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 == 0) bufferPointer[i] = bufferPointer[i] & 0x00;
        break;
      default:
        /* 功能：动画收尾 */
        fadeFlag = 0;
        break;
    }
  else
    switch (fadeFlag) {
      case 1:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 != 0) bufferPointer[i] = bufferPointer[i] | 0xAA;
        break;
      case 2:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 != 0) bufferPointer[i] = bufferPointer[i] | 0x00;
        break;
      case 3:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 == 0) bufferPointer[i] = bufferPointer[i] | 0x55;
        break;
      case 4:
        for (uint16_t i = 0; i < bufferLen; ++i) if (i % 2 == 0) bufferPointer[i] = bufferPointer[i] | 0x00;
        break;
      default:
        fadeFlag = 0;
        break;
    }
  fadeFlag++;
}

inline void Animation::blur() {
  static uint8_t bufferLen = 8 * HAL::getBufferTileHeight() * HAL::getBufferTileWidth();
  static auto *bufferPointer = (uint8_t *) HAL::getCanvasBuffer();

  for (uint16_t i = 0; i < bufferLen; ++i) bufferPointer[i] = bufferPointer[i] & (i % 2 == 0 ? 0x55 : 0xAA);
}

inline void Animation::animation(float *_pos, float _posTrg, float _speed) {
  if (*_pos != _posTrg) {
    /* 功能：差不到一像素就到位 */
    if (std::fabs(*_pos - _posTrg) < 1.0f) *_pos = _posTrg;
    else *_pos += (_posTrg - *_pos) / ((100 - _speed) / 1.0f);
  }
}

/* 功能：菜单页基类 */
class Menu : public Item, public Animation {
public:
  /* 功能：在父页里的位置 */
  typedef struct Position {
    /* 功能：坐标都取左上角 */
    float x, xTrg;
    float y, yTrg;
  } Position;

  Position position{};

  /* 功能：前景元素坐标 */
  typedef struct PositionForeground {
    float hBar, hBarTrg;  /* 功能：进度条高度 */
    float wBar, wBarTrg;  /* 功能：进度条宽度 */
    float xBar, xBarTrg;  /* 功能：进度条横坐标 */
    float yBar, yBarTrg;  /* 功能：进度条纵坐标 */

    float yArrow, yArrowTrg;
    float yDottedLine, yDottedLineTrg;
  } PositionForeground;

  PositionForeground positionForeground{};

  std::string title;
  std::vector<uint8_t> pic;

  typedef enum PageType {
    TILE = 0,
    LIST,
  } PageType;

  PageType selfType;
  PageType childType; /* 功能：第一次加元素时定死 */

public:
  Menu *parent;
  std::vector<Menu *> child;
  uint8_t selectIndex;

  /* 功能：顶栏以下的才画 */
  float clipTop = 0;

  /* 功能：这页不画选中框 */
  bool hideSelector = false;

  /* 功能：空页面也允许打开 */
  bool openableWhenEmpty = false;

  virtual ~Menu() = default;  /* 功能：删页面走这里 */

  explicit Menu(std::string _title);
  Menu(std::string _title, std::vector<uint8_t> _pic);

  void init(std::vector<float> _camera); /* 功能：开页面时调一次 */
  void deInit(); /* 功能：关页面时调一次 */

  /* 功能：子类可自己重写画法 */
  virtual void render(std::vector<float> _camera);  /* 功能：把整页画出来 */

  /* 功能：页面自己处理 OK 键 */
  virtual bool onOkKey() { return false; }

  /* 功能：页面自己处理左右键 */
  virtual bool onLeftKey() { return false; }
  virtual bool onRightKey() { return false; }

  /* 功能：进出页面时通知一声 */
  virtual void onEnter() {}
  virtual void onExit() {}

  [[nodiscard]] uint8_t getItemNum() const;
  [[nodiscard]] Position getItemPosition(uint8_t _index) const;
  [[nodiscard]] Menu* getNext() const;  /* 功能：取下一个页面 */
  [[nodiscard]] Menu* getPreview() const;

  /* 功能：选中项由启动器改 */

  /* 功能：往页面加一行 */
  bool addItem(Menu* _page);
};

/* 功能：选中框 */
class Selector : public Item, public Animation {
private:
  Menu* menu;

public:
  /* 功能：选中框的坐标 */
  float x, xTrg;
  float y, yTrg;

  float w, wTrg;
  float h, hTrg;

  float yText, yTextTrg;  /* 功能：磁贴标题纵坐标 */

  Selector() = default;
  /* 功能：磁贴的框和字一起动 */

  std::vector<float> getPosition();

  void go(uint8_t _index);

  bool inject(Menu* _menu); /* 功能：接上要画的页面 */
  bool destroy(); /* 功能：把页面删掉 */

  void render(std::vector<float> _camera);
};

/* 功能：镜头动，前景不动 */
/* 功能：画面镜头 */
class Camera : public Item, public Animation {
private:
  float xInit, yInit;

public:
  float x, y;

  bool moving = false;

  /* 功能：造个空镜头 */
  Camera();
  /* 功能：带位置造镜头 */
  Camera(float _x, float _y);

  /* 功能：顶栏下沿算上界 */
  uint8_t outOfView(float _x, float _y, float _clipTop = 0);
  std::vector<float> getPosition();

  /* 功能：挪镜头换视角 */
  void go(float _x, float _y);
  void goDirect(float _x, float _y);
  void goHorizontal(float _x);
  void goVertical(float _y);

  void goToNextPageItem();
  void goToPreviewPageItem();
  void goToListItemPage(uint8_t _index);
  void goToListItemRolling(std::vector<float> _posSelector, float _clipTop);
  void goToTileItem(uint8_t _index);

  bool isMoving();

  void reset();

  void update(Menu *_menu, Selector *_selector);
};

}

#endif