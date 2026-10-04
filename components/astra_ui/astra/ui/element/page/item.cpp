/*
 * 模块：
 *   界面的零件实现。页面怎么摆、选中框怎么动、镜头怎么滚都写在这，
 *   被 launcher.cpp 每帧调，向下用 hal 画屏，
 *   页面里的文字由 astra_rocket.cpp 填。
 *
 * 功能：
 *   造页面和零件
 *   摆位置
 *   画出来
 */

/* 功能：一页就是一行行内容 */

#include "item.h"

#include <utility>

namespace astra {

/* 功能：把参数取到自己身上 */
void Item::updateConfig() {
  this->systemConfig = HAL::getSystemConfig();
  this->astraConfig = getUIConfig();
}

/* 功能：造一个列表页 */
Menu::Menu(std::string _title) {
  this->title = std::move(_title);
  this->selfType = LIST;
  this->childType = {};
  this->position.x = astraConfig.listTextMargin;
  this->position.y = 0;
  this->position.xTrg = astraConfig.listTextMargin;
  this->position.yTrg = 0;  /* 功能：位置等加行时再算 */
  this->selectIndex = 0;
  this->parent = nullptr;
  this->child.clear();
  this->pic.clear();
}

/* 功能：造一个磁贴页 */
Menu::Menu(std::string _title, std::vector<uint8_t> _pic) {
  this->title = std::move(_title);
  this->pic = std::move(_pic);
  this->selfType = TILE;
  this->childType = {};
  this->position.x = 0;
  this->position.y = 0;
  this->position.xTrg = 0;  /* 功能：位置等加行时再算 */
  this->position.yTrg = astraConfig.tilePicTopMargin;
  this->selectIndex = 0;
  this->parent = nullptr;
  this->child.clear();
}

/* 功能：页面进场摆位 */
void Menu::init(std::vector<float> _camera) {
  entryAnimation();

  if (childType == TILE) {
    /* 功能：开合开关定起点 */
    if (astraConfig.tileUnfold) {
      for (auto _iter : child) _iter->position.x = _camera[0] - astraConfig.tilePicWidth; /* 功能：从左边滑进来 */
      positionForeground.wBar = 0;  /* 功能：进度条从零长 */

    } else {
      for (auto _iter : child) _iter->position.x = _iter->position.xTrg;
      positionForeground.wBar = positionForeground.wBarTrg;
    }

    /* 功能：箭头虚线从底下过来 */
    positionForeground.yArrow = systemConfig.screenHeight;
    positionForeground.yDottedLine = systemConfig.screenHeight;

    /* 功能：进度条从上方滑入 */
    positionForeground.yBar = 0 - astraConfig.tileBarHeight; /* 功能：从屏外滑进来 */

  } else if (childType == LIST) {
    /* 功能：开合开关定起点 */
    if (astraConfig.listUnfold) {
      for (auto _iter : child) _iter->position.y = _camera[1] - astraConfig.listLineHeight; /* 功能：文字从上面滑下 */
      positionForeground.hBar = 0;  /* 功能：进度条从零长 */
    } else {
      for (auto _iter : child) _iter->position.y = _iter->position.yTrg;
      positionForeground.hBar = positionForeground.hBarTrg;
    }

    /* 功能：进度条起点在右 */
    positionForeground.xBar = systemConfig.screenWeight;
  }
}

/* 功能：页面收尾 */
void Menu::deInit() {
  /* 功能：先做个退场动画 */
  exitAnimation();
}

/* 功能：把整页画出来 */
void Menu::render(std::vector<float> _camera) {
  if (childType == TILE) {
    Item::updateConfig();

    /* 功能：空页面直接返回 */
    if (child.empty()) return;

    HAL::setDrawType(1);

    /* 功能：画每张图 */
    for (auto _iter : child) {
      HAL::drawBMP(_iter->position.x + _camera[0], astraConfig.tilePicTopMargin + _camera[1], astraConfig.tilePicWidth, astraConfig.tilePicHeight, _iter->pic.data());
      /* 功能：目标位置早定好了 */
      animation(&_iter->position.x, _iter->position.xTrg, astraConfig.tileAnimationSpeed);
    }

    /* 功能：顶上画进度条 */
    positionForeground.wBarTrg = (selectIndex + 1) * ((float)systemConfig.screenWeight / getItemNum());
    HAL::drawBox(0, positionForeground.yBar, positionForeground.wBar, astraConfig.tileBarHeight);

    /* 功能：画左箭头 */
    HAL::drawHLine(astraConfig.tileArrowMargin, positionForeground.yArrow, astraConfig.tileArrowWidth);
    HAL::drawPixel(astraConfig.tileArrowMargin + 1, positionForeground.yArrow + 1);
    HAL::drawPixel(astraConfig.tileArrowMargin + 2, positionForeground.yArrow + 2);
    HAL::drawPixel(astraConfig.tileArrowMargin + 1, positionForeground.yArrow - 1);
    HAL::drawPixel(astraConfig.tileArrowMargin + 2, positionForeground.yArrow - 2);

    /* 功能：画右箭头 */
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.tileArrowWidth - astraConfig.tileArrowMargin, positionForeground.yArrow, astraConfig.tileArrowWidth);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth, positionForeground.yArrow + 1);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth - 1, positionForeground.yArrow + 2);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth, positionForeground.yArrow - 1);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth - 1, positionForeground.yArrow - 2);

    /* 功能：画左边按钮 */
    HAL::drawHLine(astraConfig.tileBtnMargin, positionForeground.yArrow + 2, 9);
    HAL::drawBox(astraConfig.tileBtnMargin + 2, positionForeground.yArrow + 2 - 4, 5, 4);

    /* 功能：画右边按钮 */
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.tileBtnMargin - 9, positionForeground.yArrow + 2, 9);
    HAL::drawBox(systemConfig.screenWeight - astraConfig.tileBtnMargin - 9 + 2, positionForeground.yArrow + 2 - 4, 5, 4);

    /* 功能：画虚线 */
    HAL::drawHDottedLine(0, positionForeground.yDottedLine, systemConfig.screenWeight);

    animation(&positionForeground.yDottedLine, positionForeground.yDottedLineTrg, astraConfig.tileAnimationSpeed);
    animation(&positionForeground.yArrow, positionForeground.yArrowTrg, astraConfig.tileAnimationSpeed);

    animation(&positionForeground.wBar, positionForeground.wBarTrg, astraConfig.tileAnimationSpeed);
    animation(&positionForeground.yBar, positionForeground.yBarTrg, astraConfig.tileAnimationSpeed);
  } else if (childType == LIST) {
    Item::updateConfig();

    /* 功能：空列表直接返回 */
    if (child.empty()) return;

    HAL::setDrawType(1);

    /* 功能：超出屏幕也不怕 */
    for (size_t i = 0; i < child.size(); i++) {
      Menu *_iter = child[i];
      /* 功能：目标位置早定好了 */
      animation(&_iter->position.y, _iter->position.yTrg, astraConfig.listAnimationSpeed);
      /* 功能：顶栏下的才画 */
      if (_iter->position.y + _camera[1] < clipTop) continue;
      float x = _iter->position.x + _camera[0];
      float y = _iter->position.y + astraConfig.listTextHeight + astraConfig.listTextMargin + _camera[1];

      /* 功能：长文字来回滚 */
      if (i == (size_t)selectIndex) {
        uint16_t w = HAL::getFontWidthU16(_iter->title);
        /* 功能：宽度扣掉滚动条 */
        float avail = (float)systemConfig.screenWeight - x - astraConfig.listTextMargin
                      - astraConfig.listBarWeight;
        if (w > avail) {
          static const uint16_t kGap = 16;          /* 功能：两段之间留缝 */
          static const uint32_t kSpeedMs = 20;      /* 功能：一像素走多久 */
          float off = (float)((HAL::millis() / kSpeedMs) % (uint32_t)(w + kGap));
          HAL::drawChinese(x - off, y, _iter->title);
          HAL::drawChinese(x - off + (float)w + kGap, y, _iter->title);
          continue;
        }
      }
      HAL::drawChinese(x, y, _iter->title);
    }

    /* 功能：画右侧进度条 */
    positionForeground.hBarTrg = (selectIndex + 1) * ((float)systemConfig.screenHeight / getItemNum());
    /* 功能：画刻度线 */
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.listBarWeight, 0, astraConfig.listBarWeight);
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.listBarWeight, systemConfig.screenHeight - 1, astraConfig.listBarWeight);
    HAL::drawVLine(systemConfig.screenWeight - ceil((float) astraConfig.listBarWeight / 2.0f), 0, systemConfig.screenHeight);
    /* 功能：画进度条 */
    HAL::drawBox(positionForeground.xBar, 0, astraConfig.listBarWeight, positionForeground.hBar);

    /* 功能：亮色模式反色 */
    if (astraConfig.lightMode) {
      HAL::setDrawType(2);
      HAL::drawBox(0, 0, systemConfig.screenWeight, systemConfig.screenHeight);
      HAL::setDrawType(1);
    }

    animation(&positionForeground.hBar, positionForeground.hBarTrg, astraConfig.listAnimationSpeed);
    animation(&positionForeground.xBar, positionForeground.xBarTrg, astraConfig.listAnimationSpeed);
  }
}

/* 功能：取行数 */
uint8_t Menu::getItemNum() const {
  return child.size();
}

Menu::Position Menu::getItemPosition(uint8_t _index) const {
  return child[_index]->position;
}

Menu *Menu::getNext() const {
/* 功能：空列表别越界 */
  if (child.empty()) return nullptr;
  return child[selectIndex];
}

Menu *Menu::getPreview() const {
  return parent;
}

/* 功能：往页面加一行 */
bool Menu::addItem(Menu *_page) {
  if (_page == nullptr) return false;
  else {
    /* 功能：第一行的类型定死 */
    if (this->child.empty()) this->childType = _page->selfType;

    if (this->childType == _page->selfType) {
      _page->parent = this;
      this->child.push_back(_page);
      if (this->childType == LIST) {
        _page->position.xTrg = astraConfig.listTextMargin;
        _page->position.yTrg = (getItemNum() - 1) * astraConfig.listLineHeight;

        positionForeground.xBarTrg = systemConfig.screenWeight - astraConfig.listBarWeight;
      }
      if (this->childType == TILE) {
        _page->position.xTrg = systemConfig.screenWeight / 2 - astraConfig.tilePicWidth / 2 + (this->getItemNum() - 1) * (astraConfig.tilePicMargin + astraConfig.tilePicWidth);
        _page->position.yTrg = astraConfig.tilePicTopMargin;

        positionForeground.yBarTrg = 0;
        positionForeground.yArrowTrg = systemConfig.screenHeight - astraConfig.tileArrowBottomMargin;
        positionForeground.yDottedLineTrg = systemConfig.screenHeight - astraConfig.tileDottedLineBottomMargin;
      }
      return true;
    } else return false;
  }
}

/* 功能：把选中框挪过去 */
void Selector::go(uint8_t _index) {
  Item::updateConfig();

  /* 功能：空列表不动 */
  if (menu == nullptr || menu->getItemNum() == 0) return;

  menu->selectIndex = _index;

  /* 功能：这里改目标位置 */
  if (menu->childType == Menu::TILE) {
      xTrg = menu->child[_index]->position.xTrg - astraConfig.tileSelectBoxMargin;
      yTrg = menu->child[_index]->position.yTrg - astraConfig.tileSelectBoxMargin;

      yText = systemConfig.screenHeight; /* 功能：标题从下面滑上来 */
      yTextTrg = systemConfig.screenHeight - astraConfig.tileTextBottomMargin;

      wTrg = astraConfig.tileSelectBoxWidth;
      hTrg = astraConfig.tileSelectBoxHeight;
  } else if (menu->childType == Menu::LIST) {
      xTrg = menu->child[_index]->position.xTrg - astraConfig.selectorMargin;
      yTrg = menu->child[_index]->position.yTrg;

      /* 功能：框别超出屏幕 */
      wTrg = (float)HAL::getFontWidthU16(menu->child[_index]->title) + astraConfig.listTextMargin * 2;
      float maxW = (float)systemConfig.screenWeight - astraConfig.selectorMargin * 2;
      if (wTrg > maxW) wTrg = maxW;
      hTrg = astraConfig.listLineHeight;
  }
}

/* 功能：接上要画的页面 */
bool Selector::inject(Menu *_menu) {
  if (_menu == nullptr) return false;

  this->menu = _menu;

  /* 功能：空页也接管指针 */
  if (_menu->getItemNum() == 0) return true;

  go(this->menu->selectIndex);  /* 功能：先摆好选中框 */

  return true;
}

/* 功能：把页面删掉 */
bool Selector::destroy() {
  if (this->menu == nullptr) return false;

  delete this->menu;
  this->menu = nullptr;
  return true;
}

/* 功能：把选中框画出来 */
void Selector::render(std::vector<float> _camera) {
  Item::updateConfig();

  /* 功能：空页不画框 */
  if (menu == nullptr || menu->getItemNum() == 0) return;
  /* 功能：这页不画选中框 */
  if (menu->hideSelector) return;

  /* 功能：挪的时候带过渡 */
  animation(&x, xTrg, astraConfig.selectorXAnimationSpeed);
  animation(&y, yTrg, astraConfig.selectorYAnimationSpeed);
  animation(&h, hTrg, astraConfig.selectorHeightAnimationSpeed);
  animation(&w, wTrg, astraConfig.selectorWidthAnimationSpeed);

  if (menu->childType == Menu::TILE) {
    animation(&yText, yTextTrg, astraConfig.selectorYAnimationSpeed);

    /* 功能：标题不随镜头动 */
    HAL::setDrawType(1);
    HAL::drawChinese((systemConfig.screenWeight - (float)HAL::getFontWidth(menu->child[menu->selectIndex]->title)) / 2.0, yText + astraConfig.tileTitleHeight, menu->child[menu->selectIndex]->title);

    /* 功能：大框跟着镜头动 */
    HAL::setDrawType(2);
    HAL::drawPixel(x + _camera[0], y + _camera[1]);
    /* 功能：左上角 */
    HAL::drawHLine(x + _camera[0], y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    HAL::drawVLine(x + _camera[0], y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    /* 功能：左下角 */
    HAL::drawHLine(x + _camera[0], y + _camera[1] + h - 1, astraConfig.tileSelectBoxLineLength + 1);
    HAL::drawVLine(x + _camera[0], y + _camera[1] + h - astraConfig.tileSelectBoxLineLength - 1, astraConfig.tileSelectBoxLineLength);
    /* 功能：右上角 */
    HAL::drawHLine(x + _camera[0] + w - astraConfig.tileSelectBoxLineLength - 1, y + _camera[1], astraConfig.tileSelectBoxLineLength);
    HAL::drawVLine(x + _camera[0] + w - 1, y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    /* 功能：右下角 */
    HAL::drawHLine(x + _camera[0] + w - astraConfig.tileSelectBoxLineLength - 1, y + _camera[1] + h - 1, astraConfig.tileSelectBoxLineLength);
    HAL::drawVLine(x + _camera[0] + w - 1, y + _camera[1] + h - astraConfig.tileSelectBoxLineLength - 1, astraConfig.tileSelectBoxLineLength);

    HAL::drawPixel(x + _camera[0] + w - 1, y + _camera[1] + h - 1);
  } else if (menu->childType == Menu::LIST) {

    /* 功能：顶栏下才画框 */
    if (y + _camera[1] < menu->clipTop) return;

    /* 功能：框跟着镜头动 */
    HAL::setDrawType(2);
    HAL::drawRBox(x + _camera[0], y + _camera[1], w, h - 1, astraConfig.selectorRadius);
    HAL::setDrawType(1);
  }
}

std::vector<float> Selector::getPosition() {
  return {xTrg, yTrg};
}

/* 功能：造个空镜头 */
Camera::Camera() {
  this->xInit = 0;
  this->yInit = 0;

  this->x = 0;
  this->y = 0;
}

/* 功能：坐标取反来挪视角 */

/* 功能：带位置造镜头 */
Camera::Camera(float _x, float _y) {
  this->xInit = 0 - _x;
  this->yInit = 0 - _y;

  this->x = 0 - _x;
  this->y = 0 - _y;
}

/* 功能：看在不在视野里 */
uint8_t Camera::outOfView(float _x, float _y, float _clipTop) {
  if ((_x < 0 - this->x) || (_y < _clipTop - this->y)) return 1;
  if ((_x > (0 - this->x) + systemConfig.screenWeight - 1) ||
      (_y > (0 - this->y) + systemConfig.screenHeight - 1)) return 2;
  return 0;
}

std::vector<float> Camera::getPosition() {
  return {x, y};
}

/* 功能：慢慢挪到目标点 */
void Camera::go(float _x, float _y) {
  moving = true;
  animation(&this->x, (0 - _x), astraConfig.cameraAnimationSpeed);
  animation(&this->y, (0 - _y), astraConfig.cameraAnimationSpeed);
  if (this->x == 0 - _x && this->y == 0 - _y) moving = false;
}

/* 功能：一步到位不动画 */
void Camera::goDirect(float _x, float _y) {
  this->x = 0 - _x;
  this->y = 0 - _y;
}

void Camera::goHorizontal(float _x) {
  moving = true;
  animation(&this->x, 0 - _x, astraConfig.cameraAnimationSpeed);
  if (this->x == 0 - _x) moving = false;
}

void Camera::goVertical(float _y) {
  moving = true;
  animation(&this->y, 0 - _y, astraConfig.cameraAnimationSpeed);
  if (this->y == 0 - _y) moving = false;
}

void Camera::goToNextPageItem() {
  moving = true;
  animation(&y, y - systemConfig.screenHeight, astraConfig.cameraAnimationSpeed);
  if (this->y == y - systemConfig.screenHeight) moving = false;
}

void Camera::goToPreviewPageItem() {
  moving = true;
  animation(&y, y + systemConfig.screenHeight, astraConfig.cameraAnimationSpeed);
  if (this->y == y + systemConfig.screenHeight) moving = false;
}

/* 功能：整页翻过去 */
void Camera::goToListItemPage(uint8_t _index) {
  static const uint8_t maxItemPerScreen = systemConfig.screenHeight / astraConfig.listLineHeight;
  uint8_t _page = 0;

  moving = true;

  if (_index == 0) _page = 0;
  else if (_index % maxItemPerScreen == 0) _page = _index / maxItemPerScreen;
  else _page = floor(_index / maxItemPerScreen);
  go(0, _page * systemConfig.screenHeight);

  if (this->y == _page * systemConfig.screenHeight) moving = false;
}

void Camera::goToListItemRolling(std::vector<float> _posSelector, float _clipTop) {

  /* 功能：选到屏外就滚一行 */

  static uint8_t direction = 0; /* 功能：0不动 1上 2下 */

  /* 功能：最多滚到顶栏下 */
  moving = true;
  if (outOfView(_posSelector[0], _posSelector[1], _clipTop) == 1) direction = 1;
  if (outOfView(_posSelector[0], _posSelector[1], _clipTop) == 2) direction = 2;

  if (direction == 1) {
    go(_posSelector[0], _posSelector[1] - _clipTop);
    if (this->x == 0 - _posSelector[0] && this->y == 0 - (_posSelector[1] - _clipTop)) direction = 0;
  }
  if (direction == 2) {
    go(_posSelector[0], _posSelector[1] + astraConfig.listLineHeight - systemConfig.screenHeight);
    if (this->x == 0 - _posSelector[0] && this->y == 0 - (_posSelector[1] + astraConfig.listLineHeight - systemConfig.screenHeight)) direction = 0;
  }

  if (!outOfView(_posSelector[0], _posSelector[1], _clipTop)) moving = false;
}

void Camera::goToTileItem(uint8_t _index) {
  moving = true;
  go(_index * (astraConfig.tilePicWidth + astraConfig.tilePicMargin), 0);
  if (this->x == 0 - _index * (astraConfig.tilePicWidth + astraConfig.tilePicMargin)) moving = false;
}

bool Camera::isMoving() {
  return moving;
}

/* 功能：视角回原位 */
void Camera::reset() {
  moving = true;
  animation(&this->x, xInit, astraConfig.cameraAnimationSpeed);
  animation(&this->y, yInit, astraConfig.cameraAnimationSpeed);
  if (this->x == xInit && this->y == yInit) moving = false;
}

/* 功能：按选中行调视角 */
void Camera::update(Menu *_menu, Selector *_selector) {
  /* 功能：按页型决定滚动 */
  if (_menu->childType == Menu::LIST) {
    /* 功能：空列表不滚 */
    if (_menu->getItemNum() == 0) return;
    if (astraConfig.listPageTurningMode == 0) goToListItemPage(_menu->selectIndex);
    else if (astraConfig.listPageTurningMode == 1) goToListItemRolling(_selector->getPosition(), _menu->clipTop);
  }
  else if (_menu->childType == Menu::TILE) goToTileItem(_menu->selectIndex);
}

}
