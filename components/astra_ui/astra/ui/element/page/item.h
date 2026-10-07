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

// 零件基类
class Item {
protected:
  sys::config systemConfig;
  config astraConfig;

  void updateConfig();
};

// 动画基类
class Animation {
public:
  virtual void entryAnimation();
  virtual void exitAnimation();
  virtual void blur();
  virtual void animation(float *_pos, float _posTrg, float _speed);
};

inline void Animation::entryAnimation() { }

// 退场时闪一下屏
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
        // 动画收尾
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
    // 差不到一像素就到位
    if (std::fabs(*_pos - _posTrg) < 1.0f) *_pos = _posTrg;
    else *_pos += (_posTrg - *_pos) / ((100 - _speed) / 1.0f);
  }
}

// 菜单页基类
class Menu : public Item, public Animation {
public:
  // 在父页里的位置
  typedef struct Position {
    // 坐标都取左上角
    float x, xTrg;
    float y, yTrg;
  } Position;

  Position position{};

  // 前景元素坐标
  typedef struct PositionForeground {
    // 进度条高度
    float hBar, hBarTrg;
    // 进度条宽度
    float wBar, wBarTrg;
    // 进度条横坐标
    float xBar, xBarTrg;
    // 进度条纵坐标
    float yBar, yBarTrg;

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
  // 第一次加元素时定死
  PageType childType;

public:
  Menu *parent;
  std::vector<Menu *> child;
  uint8_t selectIndex;

  // 顶栏以下的才画
  float clipTop = 0;

  // 这页不画选中框
  bool hideSelector = false;

  // 空页面也允许打开
  bool openableWhenEmpty = false;

  // 删页面走这里
  virtual ~Menu() = default;

  explicit Menu(std::string _title);
  Menu(std::string _title, std::vector<uint8_t> _pic);

  // 开页面时调一次
  void init(std::vector<float> _camera);
  // 关页面时调一次
  void deInit();

  // 子类可自己重写画法
  // 把整页画出来
  virtual void render(std::vector<float> _camera);

  // 页面自己处理 OK 键
  virtual bool onOkKey() { return false; }

  // 页面自己处理左右键
  virtual bool onLeftKey() { return false; }
  virtual bool onRightKey() { return false; }

  // 进出页面时通知一声
  virtual void onEnter() {}
  virtual void onExit() {}

  [[nodiscard]] uint8_t getItemNum() const;
  [[nodiscard]] Position getItemPosition(uint8_t _index) const;
  // 取下一个页面
  [[nodiscard]] Menu* getNext() const;
  [[nodiscard]] Menu* getPreview() const;

  // 选中项由启动器改

  // 往页面加一行
  bool addItem(Menu* _page);
};

// 选中框
class Selector : public Item, public Animation {
private:
  Menu* menu;

public:
  // 选中框的坐标
  float x, xTrg;
  float y, yTrg;

  float w, wTrg;
  float h, hTrg;

  // 磁贴标题纵坐标
  float yText, yTextTrg;

  Selector() = default;
  // 磁贴的框和字一起动

  std::vector<float> getPosition();

  void go(uint8_t _index);

  // 接上要画的页面
  bool inject(Menu* _menu);
  // 把页面删掉
  bool destroy();

  void render(std::vector<float> _camera);
};

// 镜头动，前景不动
// 画面镜头
class Camera : public Item, public Animation {
private:
  float xInit, yInit;

public:
  float x, y;

  bool moving = false;

  // 造个空镜头
  Camera();
  // 带位置造镜头
  Camera(float _x, float _y);

  // 顶栏下沿算上界
  uint8_t outOfView(float _x, float _y, float _clipTop = 0);
  std::vector<float> getPosition();

  // 挪镜头换视角
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