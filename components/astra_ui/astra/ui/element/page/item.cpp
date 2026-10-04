//
// Created by Fir on 2024/1/21.
//

//每个列表应该有 1. 文本（需要显示的内容） 2. 跳转向量表（对应1，每一行都要跳转到哪里）
//其中文本以string形式 向量表以vector形式 以类的形式整合在一起 编写相应的构造函数 调用构造函数即是新建一个页面
//通过最后一项跳转向量表可以判断其类型 比如跳转至下一页 或者返回上一级 或者是复选框 或者是弹窗 或者是侧边栏弹窗 等等
//做一个astra类作为总的框架 在ui命名空间里

//分为
//驱动层 - 界面层（包括各种列表 磁铁 复选框 侧边栏 弹窗等） - 处理层（各种坐标的变换等）

//传进去的有三个东西 第一个是文字（要显示的东西） 第二个是类别（普通列表 跳转列表 复选框） 第三个是特征向量（指向复选框和弹窗对应的参数 要跳转的地方等）

////todo 小组件下个版本更新

#include "item.h"

#include <utility>

/**
 *     ·  ·     ·   ·
 *  ·   ·  ·  ·   ·
 *  循此苦旅，直抵群星。
 *  ad astra per aspera.
 * happy Chinese new year!
 *      新年快乐！
 * ·   ·   ·      ·
 *   ·   ·    · ·   ·
 */

namespace astra {

void Item::updateConfig() {
  this->systemConfig = HAL::getSystemConfig();
  this->astraConfig = getUIConfig();
}

Menu::Menu(std::string _title) {
  this->title = std::move(_title);
  this->selfType = LIST;
  this->childType = {};
  this->position.x = astraConfig.listTextMargin;
  this->position.y = 0;
  this->position.xTrg = astraConfig.listTextMargin;
  this->position.yTrg = 0;  //这里暂时无法计算trg 需要在addItem的时候计算 因为那时候才能拿到所有元素的数量
  this->selectIndex = 0;
  this->parent = nullptr;
  this->child.clear();
  this->pic.clear();
}

Menu::Menu(std::string _title, std::vector<uint8_t> _pic) {
  this->title = std::move(_title);
  this->pic = std::move(_pic);
  this->selfType = TILE;
  this->childType = {};
  this->position.x = 0;
  this->position.y = 0;
  this->position.xTrg = 0;  //这里暂时无法计算trg 需要在addItem的时候计算 因为那时候才能拿到所有元素的数量
  this->position.yTrg = astraConfig.tilePicTopMargin;
  this->selectIndex = 0;
  this->parent = nullptr;
  this->child.clear();
}

void Menu::init(std::vector<float> _camera) {
  entryAnimation();

  if (childType == TILE) {
    //受展开开关影响的坐标初始化
    if (astraConfig.tileUnfold) {
      for (auto _iter : child) _iter->position.x = _camera[0] - astraConfig.tilePicWidth; //unfold from left.
      positionForeground.wBar = 0;  //bar unfold from left.

    } else {
      for (auto _iter : child) _iter->position.x = _iter->position.xTrg;
      positionForeground.wBar = positionForeground.wBarTrg;
    }

    //position.y = -astraConfig.tilePicHeight * 2;

    //始终执行的坐标初始化
    //底部箭头和虚线的初始化
    positionForeground.yArrow = systemConfig.screenHeight;
    positionForeground.yDottedLine = systemConfig.screenHeight;

    //顶部进度条的从上方滑入的初始化
    positionForeground.yBar = 0 - astraConfig.tileBarHeight; //注意这里是坐标从屏幕外滑入 而不是height从0变大

  } else if (childType == LIST) {
    //受展开开关影响的坐标初始化
    if (astraConfig.listUnfold) {
      for (auto _iter : child) _iter->position.y = _camera[1] - astraConfig.listLineHeight; //text unfold from top.
      positionForeground.hBar = 0;  //bar unfold from top.
    } else {
      for (auto _iter : child) _iter->position.y = _iter->position.yTrg;
      positionForeground.hBar = positionForeground.hBarTrg;
    }

    //始终执行的坐标初始化
    positionForeground.xBar = systemConfig.screenWeight;
  }
}

void Menu::deInit() {
  //todo 未实现完全
  exitAnimation();
}

void Menu::render(std::vector<float> _camera) {
  if (childType == TILE) {
    Item::updateConfig();

    // ESP32 移植：空 TILE 页防护——进度条计算 getItemNum()=0 会除零
    // 产生 inf/NaN 坐标 → u8g2 绘制死循环 → 看门狗复位
    if (child.empty()) return;

    HAL::setDrawType(1);

    //draw pic.
    for (auto _iter : child) {
      HAL::drawBMP(_iter->position.x + _camera[0], astraConfig.tilePicTopMargin + _camera[1], astraConfig.tilePicWidth, astraConfig.tilePicHeight, _iter->pic.data());
      //这里的xTrg在addItem的时候就已经确定了
      animation(&_iter->position.x, _iter->position.xTrg, astraConfig.tileAnimationSpeed);
    }

    //draw bar.
    //在屏幕最上方 两个像素高
    positionForeground.wBarTrg = (selectIndex + 1) * ((float)systemConfig.screenWeight / getItemNum());
    HAL::drawBox(0, positionForeground.yBar, positionForeground.wBar, astraConfig.tileBarHeight);

    //draw left arrow.
    HAL::drawHLine(astraConfig.tileArrowMargin, positionForeground.yArrow, astraConfig.tileArrowWidth);
    HAL::drawPixel(astraConfig.tileArrowMargin + 1, positionForeground.yArrow + 1);
    HAL::drawPixel(astraConfig.tileArrowMargin + 2, positionForeground.yArrow + 2);
    HAL::drawPixel(astraConfig.tileArrowMargin + 1, positionForeground.yArrow - 1);
    HAL::drawPixel(astraConfig.tileArrowMargin + 2, positionForeground.yArrow - 2);

    //draw right arrow.
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.tileArrowWidth - astraConfig.tileArrowMargin, positionForeground.yArrow, astraConfig.tileArrowWidth);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth, positionForeground.yArrow + 1);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth - 1, positionForeground.yArrow + 2);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth, positionForeground.yArrow - 1);
    HAL::drawPixel(systemConfig.screenWeight - astraConfig.tileArrowWidth - 1, positionForeground.yArrow - 2);

    //draw left button.
    HAL::drawHLine(astraConfig.tileBtnMargin, positionForeground.yArrow + 2, 9);
    HAL::drawBox(astraConfig.tileBtnMargin + 2, positionForeground.yArrow + 2 - 4, 5, 4);

    //draw right button.
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.tileBtnMargin - 9, positionForeground.yArrow + 2, 9);
    HAL::drawBox(systemConfig.screenWeight - astraConfig.tileBtnMargin - 9 + 2, positionForeground.yArrow + 2 - 4, 5, 4);

    //draw dotted line.
    HAL::drawHDottedLine(0, positionForeground.yDottedLine, systemConfig.screenWeight);

    animation(&positionForeground.yDottedLine, positionForeground.yDottedLineTrg, astraConfig.tileAnimationSpeed);
    animation(&positionForeground.yArrow, positionForeground.yArrowTrg, astraConfig.tileAnimationSpeed);

    animation(&positionForeground.wBar, positionForeground.wBarTrg, astraConfig.tileAnimationSpeed);
    animation(&positionForeground.yBar, positionForeground.yBarTrg, astraConfig.tileAnimationSpeed);
  } else if (childType == LIST) {
    Item::updateConfig();

    // ESP32 移植：空列表防护。列表页允许 0 个子项（例如空域无无人机），
    // 此时跳过进度条计算（除零）与绘制，由派生页面的自定义 render 显示空态
    if (child.empty()) return;

    HAL::setDrawType(1);

    //allow x > screen height, y > screen weight.
    for (size_t i = 0; i < child.size(); i++) {
      Menu *_iter = child[i];
      //这里的yTrg在addItem的时候就已经确定了
      animation(&_iter->position.y, _iter->position.yTrg, astraConfig.listAnimationSpeed);
      // ESP32 移植：行内容绘制位置越过顶栏上界（clipTop）时整行不画——
      // 顶栏下第一行即初始位置，滚动时行内容最多贴到顶栏下沿，绝不堆积到标题上
      if (_iter->position.y + _camera[1] < clipTop) continue;
      float x = _iter->position.x + _camera[0];
      float y = _iter->position.y + astraConfig.listTextHeight + astraConfig.listTextMargin + _camera[1];

      // ESP32 移植：选中行文字超宽自动跑马灯（日志长行等
      // 超出 128px 可读）。左右双拷贝无缝循环：前拷贝向左滑出，后拷贝
      // 从右缘跟进；负 x 由 u8g2 整体裁剪（与页面滑入动画同语义）
      if (i == (size_t)selectIndex) {
        uint16_t w = HAL::getFontWidthU16(_iter->title);
        /* 可用宽度需扣除右侧 5px 滚动指示条（行文字会与之重叠被压住） */
        float avail = (float)systemConfig.screenWeight - x - astraConfig.listTextMargin
                      - astraConfig.listBarWeight;
        if (w > avail) {
          static const uint16_t kGap = 16;          /* 首尾拷贝间距（px） */
          static const uint32_t kSpeedMs = 20;      /* 每像素滚动耗时 */
          float off = (float)((HAL::millis() / kSpeedMs) % (uint32_t)(w + kGap));
          HAL::drawChinese(x - off, y, _iter->title);
          HAL::drawChinese(x - off + (float)w + kGap, y, _iter->title);
          continue;
        }
      }
      HAL::drawChinese(x, y, _iter->title);
    }

    //draw bar.
    positionForeground.hBarTrg = (selectIndex + 1) * ((float)systemConfig.screenHeight / getItemNum());
    //画指示线
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.listBarWeight, 0, astraConfig.listBarWeight);
    HAL::drawHLine(systemConfig.screenWeight - astraConfig.listBarWeight, systemConfig.screenHeight - 1, astraConfig.listBarWeight);
    HAL::drawVLine(systemConfig.screenWeight - ceil((float) astraConfig.listBarWeight / 2.0f), 0, systemConfig.screenHeight);
    //draw bar.
    HAL::drawBox(positionForeground.xBar, 0, astraConfig.listBarWeight, positionForeground.hBar);

    //light mode.
    if (astraConfig.lightMode) {
      HAL::setDrawType(2);
      HAL::drawBox(0, 0, systemConfig.screenWeight, systemConfig.screenHeight);
      HAL::setDrawType(1);
    }

    animation(&positionForeground.hBar, positionForeground.hBarTrg, astraConfig.listAnimationSpeed);
    animation(&positionForeground.xBar, positionForeground.xBarTrg, astraConfig.listAnimationSpeed);
  }
}

uint8_t Menu::getItemNum() const {
  return child.size();
}

Menu::Position Menu::getItemPosition(uint8_t _index) const {
  return child[_index]->position;
}

Menu *Menu::getNext() const {
  // ESP32 移植：空 vector 上 operator[] 是未定义行为（实际 NULL 解引用崩溃），
  // 先判空返回 nullptr（Launcher::open 已有 nullptr 检查）
  if (child.empty()) return nullptr;
  return child[selectIndex];
}

Menu *Menu::getPreview() const {
  return parent;
}

bool Menu::addItem(Menu *_page) {
  if (_page == nullptr) return false;
  else {
    //加入的第一个元素的类型决定了当前节点所有子元素的类型
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

/**
 * @brief
 *
 * @param _index
 * @note selector接管了移动选择指针的功能
 * @warning not support in loop. 不支持在循环内执行
 */
void Selector::go(uint8_t _index) {
  Item::updateConfig();

  // ESP32 移植：空列表防护。动态列表（无人机列表）可重建为 0 项，
  // 此时无子项可定位，直接返回避免越界
  if (menu == nullptr || menu->getItemNum() == 0) return;

  menu->selectIndex = _index;

  //在go的时候改变trg的值
  if (menu->childType == Menu::TILE) {
//    xTrg = menu->child[_index]->position.xTrg - (astraConfig.tileSelectBoxWeight - astraConfig.tilePicWidth) / 2;
//    yTrg = menu->child[_index]->position.yTrg - (astraConfig.tileSelectBoxHeight - astraConfig.tilePicHeight) / 2;
      xTrg = menu->child[_index]->position.xTrg - astraConfig.tileSelectBoxMargin;
      yTrg = menu->child[_index]->position.yTrg - astraConfig.tileSelectBoxMargin;

      yText = systemConfig.screenHeight; //给磁贴文字归零 从屏幕外滑入
      yTextTrg = systemConfig.screenHeight - astraConfig.tileTextBottomMargin;

      wTrg = astraConfig.tileSelectBoxWidth;
      hTrg = astraConfig.tileSelectBoxHeight;
  } else if (menu->childType == Menu::LIST) {
      xTrg = menu->child[_index]->position.xTrg - astraConfig.selectorMargin;
      yTrg = menu->child[_index]->position.yTrg;

      /* 16 位字宽 + 钳位：长标题（日志长行等）选择框不超出屏幕右缘 */
      wTrg = (float)HAL::getFontWidthU16(menu->child[_index]->title) + astraConfig.listTextMargin * 2;
      float maxW = (float)systemConfig.screenWeight - astraConfig.selectorMargin * 2;
      if (wTrg > maxW) wTrg = maxW;
      hTrg = astraConfig.listLineHeight;
  }
}

bool Selector::inject(Menu *_menu) {
  if (_menu == nullptr) return false;

  //if (this->menu != nullptr) return false;
  this->menu = _menu;

  // ESP32 移植：空页也要接管 menu 指针（render 据此跳过绘制），
  // 但无子项时不计算选择框位置
  if (_menu->getItemNum() == 0) return true;

  go(this->menu->selectIndex);  //注入之后要初始化选择框的位置

  return true;
}

bool Selector::destroy() {
  if (this->menu == nullptr) return false;

  delete this->menu;
  this->menu = nullptr;
  return true;
}

void Selector::render(std::vector<float> _camera) {
  Item::updateConfig();

  // ESP32 移植：空列表防护。空页（例如空域无无人机）不绘制选择框
  if (menu == nullptr || menu->getItemNum() == 0) return;
  // ESP32 移植：自定义渲染页（主页面时钟）不绘制选择框
  if (menu->hideSelector) return;

  //实际上 这里已经实现过渡动画了
  animation(&x, xTrg, astraConfig.selectorXAnimationSpeed);
  animation(&y, yTrg, astraConfig.selectorYAnimationSpeed);
  animation(&h, hTrg, astraConfig.selectorHeightAnimationSpeed);
  animation(&w, wTrg, astraConfig.selectorWidthAnimationSpeed);

  if (menu->childType == Menu::TILE) {
    animation(&yText, yTextTrg, astraConfig.selectorYAnimationSpeed);

    //draw text.
    //文字不受摄像机的影响
    HAL::setDrawType(1);
    HAL::drawChinese((systemConfig.screenWeight - (float)HAL::getFontWidth(menu->child[menu->selectIndex]->title)) / 2.0, yText + astraConfig.tileTitleHeight, menu->child[menu->selectIndex]->title);

    //draw box.
    //大框需要受摄像机的影响
    HAL::setDrawType(2);
    HAL::drawPixel(x + _camera[0], y + _camera[1]);
    //左上角
    HAL::drawHLine(x + _camera[0], y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    HAL::drawVLine(x + _camera[0], y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    //左下角
    HAL::drawHLine(x + _camera[0], y + _camera[1] + h - 1, astraConfig.tileSelectBoxLineLength + 1);
    HAL::drawVLine(x + _camera[0], y + _camera[1] + h - astraConfig.tileSelectBoxLineLength - 1, astraConfig.tileSelectBoxLineLength);
    //右上角
    HAL::drawHLine(x + _camera[0] + w - astraConfig.tileSelectBoxLineLength - 1, y + _camera[1], astraConfig.tileSelectBoxLineLength);
    HAL::drawVLine(x + _camera[0] + w - 1, y + _camera[1], astraConfig.tileSelectBoxLineLength + 1);
    //右下角
    HAL::drawHLine(x + _camera[0] + w - astraConfig.tileSelectBoxLineLength - 1, y + _camera[1] + h - 1, astraConfig.tileSelectBoxLineLength);
    HAL::drawVLine(x + _camera[0] + w - 1, y + _camera[1] + h - astraConfig.tileSelectBoxLineLength - 1, astraConfig.tileSelectBoxLineLength);

    HAL::drawPixel(x + _camera[0] + w - 1, y + _camera[1] + h - 1);
  } else if (menu->childType == Menu::LIST) {
    //animation(&h, hTrg, astraConfig.selectorAnimationSpeed);

    // ESP32 移植：选择框越过顶栏上界（clipTop）时不画——
    // 与行内容裁剪一致，防止框叠在标题栏上
    if (y + _camera[1] < menu->clipTop) return;

    //draw select box.
    //受摄像机的影响
    HAL::setDrawType(2);
    HAL::drawRBox(x + _camera[0], y + _camera[1], w, h - 1, astraConfig.selectorRadius);
    //HAL::drawRBox(x, y, w, astraConfig.listLineHeight, astraConfig.selectorRadius);
    HAL::setDrawType(1);
  }
}

std::vector<float> Selector::getPosition() {
  return {xTrg, yTrg};
}

Camera::Camera() {
  this->xInit = 0;
  this->yInit = 0;

  this->x = 0;
  this->y = 0;
}

//这里的坐标应该都是负的 因为最终渲染的时候是加上摄像机的坐标
//所以说比如想显示下一页 应该是item本身的坐标减去摄像机的坐标 这样才会让item向上移动
//一个办法是用户传进来正的坐标 但是在摄像机内部 所有坐标都取其相反数 负的

Camera::Camera(float _x, float _y) {
  this->xInit = 0 - _x;
  this->yInit = 0 - _y;

  this->x = 0 - _x;
  this->y = 0 - _y;
}

/**
 *
 * @param _x
 * @param _y
 * @param _clipTop ESP32 移植：页面顶栏高度——视野上界移到顶栏下沿
 * @return 0: in view, 1: upper, 2: lower
 */
uint8_t Camera::outOfView(float _x, float _y, float _clipTop) {
  if ((_x < 0 - this->x) || (_y < _clipTop - this->y)) return 1;
  if ((_x > (0 - this->x) + systemConfig.screenWeight - 1) ||
      (_y > (0 - this->y) + systemConfig.screenHeight - 1)) return 2;
  return 0;
}

std::vector<float> Camera::getPosition() {
  return {x, y};
}

/**
 * @brief
 * @param _pos
 * @param _posTrg
 * @param _speed
 *
 * @note only support in loop. 仅支持在循环内执行
 */
void Camera::go(float _x, float _y) {
  moving = true;
  animation(&this->x, (0 - _x), astraConfig.cameraAnimationSpeed);
  animation(&this->y, (0 - _y), astraConfig.cameraAnimationSpeed);
  if (this->x == 0 - _x && this->y == 0 - _y) moving = false;
}

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

  //这是一个让页面在一定情况下向下或向上滚动一行的函数
  //当index向上超越了一个屏幕可以显示的内容 就要向上滚动一行 滚动到以当前选择项为第一项的页面
  //当index向下超越了一个屏幕可以显示的内容 就要向下滚动一行 滚动到以当前选择项为最后一项的页面
  //正常情况下 不移动

  //如果不碰边界 不更新端点
  //端点惰性赋值
  //计算出每页第一行的index和最后一行的index
  //碰到下边界就向下滚动 碰到上边界就向上滚动

  //最开始左端点是0 右端点是max-1
  //index超过右端点 就向下滚动index-右端点行 同时左右端点都加上index-右端点

  static uint8_t direction = 0; //0: no roll, 1: up, 2: down

  // ESP32 移植：视野上界为 _clipTop（顶栏下沿）。上滚目标 =
  // 选中行贴顶栏下沿（而非屏幕顶端），保证顶栏下第一行始终是
  // 完整行且滚动永不越过初始位置
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

void Camera::reset() {
  moving = true;
  animation(&this->x, xInit, astraConfig.cameraAnimationSpeed);
  animation(&this->y, yInit, astraConfig.cameraAnimationSpeed);
  if (this->x == xInit && this->y == yInit) moving = false;
}

void Camera::update(Menu *_menu, Selector *_selector) {
  //todo 不完善
  if (_menu->childType == Menu::LIST) {
    // ESP32 移植：空列表页不滚动（选择框位置无效，防止摄像机漂移）
    if (_menu->getItemNum() == 0) return;
    if (astraConfig.listPageTurningMode == 0) goToListItemPage(_menu->selectIndex);
    else if (astraConfig.listPageTurningMode == 1) goToListItemRolling(_selector->getPosition(), _menu->clipTop);
  }
  else if (_menu->childType == Menu::TILE) goToTileItem(_menu->selectIndex);
}

}
