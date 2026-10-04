//
// Created by Fir on 2024/1/25.
//

#pragma once
#ifndef ASTRA_CORE_SRC_SYSTEM_H_
#define ASTRA_CORE_SRC_SYSTEM_H_

#include "cstdint"
#include "u8g2.h"

namespace astra {
/**
 * @brief config of astra ui. astra ui的配置结构体
 */
struct config {
  // ESP32 移植：本机帧率低于原 STM32 目标（~30 FPS 全屏 / 更高局部），
  // 原速度值几何收敛过慢（尾部拖行数百帧）；整体上调速度、更快到位
  float tileAnimationSpeed = 90;
  float listAnimationSpeed = 85;
  float selectorYAnimationSpeed = 85;
  float selectorXAnimationSpeed = 88;
  float selectorWidthAnimationSpeed = 88;
  float selectorHeightAnimationSpeed = 85;
  float windowAnimationSpeed = 70;
  float sideBarAnimationSpeed = 70;
  float fadeAnimationSpeed = 100;
  float cameraAnimationSpeed = 85;
  float logoAnimationSpeed = 90;

  bool tileUnfold = true;
  bool listUnfold = true;

  bool tileLoop = true;
  bool menuLoop = true;

  bool backgroundBlur = true;
  bool lightMode = false;

  float listBarWeight = 5;
  float listTextHeight = 8;
  float listTextMargin = 4; //文字边距
  float listLineHeight = 16;
  float selectorRadius = 0.5f;
  float selectorMargin = 4; //选择框与文字左边距
  float selectorTopMargin = 2; //选择框与文字上边距

  uint8_t listPageTurningMode = 1; //0: 翻页模式 1: 滚动模式

  float tilePicWidth = 30;
  float tilePicHeight = 30;
  float tilePicMargin = 8;
  float tilePicTopMargin = 8; //图标上边距
  float tileArrowWidth = 6;
  float tileArrowMargin = 4; //箭头边距

  //todo 如果有问题 给下面这三个分别+1
  float tileDottedLineBottomMargin = 18; //虚线下边距(top: 46)
  float tileArrowBottomMargin = 8; //箭头下边距(top: 56)
  float tileTextBottomMargin = 12; //标题下边距(top: 52)

  float tileBarHeight = 2; //磁贴进度条高度

  float tileSelectBoxLineLength = 5;  //磁贴选择框线长
  float tileSelectBoxMargin = 3; //选择框边距
  float tileSelectBoxWidth = tileSelectBoxMargin * 2 + tilePicWidth; //选择框宽
  float tileSelectBoxHeight = tileSelectBoxMargin * 2 + tilePicHeight; //选择框高
  float tileTitleHeight = 8; //磁贴标题高度

  float tileBtnMargin = 16; //按钮边距

  float popMargin = 4; //弹窗边距
  float popRadius = 2; //弹窗圆角半径
  float popSpeed = 90; //弹窗动画速度

  float logoStarLength = 2; //logo星星长度
  float logoTextHeight = 14; //logo文字高度
  float logoCopyRightHeight = 8; //logo文字高度
  uint8_t logoStarNum = 16; //logo星星数量
  // ESP32 移植：原字体 u8g2_font_Cascadia / u8g2_font_myfont 为本项目 u8g2 库所无，
  // 替换为 wqy12 中文字体（含 ASCII）与 6x10 英文等宽字体
  const uint8_t *logoTitleFont = u8g2_font_wqy12_t_gb2312;
  const uint8_t *logoCopyRightFont = u8g2_font_6x10_tf;

  const uint8_t *mainFont = u8g2_font_wqy12_t_gb2312;

  // ---------------------------------------------------------------------
  //  128x64 屏的【纵向行基线】——主页 / 传感器页共用
  // ---------------------------------------------------------------------
  //  为什么要有这几个常数：本工程 u8g2 里最小的中文字体是 wqy12（行高 ≈13px），
  //  10x20_mn 大字号行高 ≈21px，64px 高的屏只放得下 4 行（标题 + 大字号 + 2 行中文）。
  //  2026-09-28 修过一个排版事故：主页把后三行画在 y=49/58/62（基线只差 4~9px），
  //  三行叠在一起，除温湿度外全糊；根因就是"凭手感给 y"，没有行高预算。
  //  现在统一从这几个常数取，并且 tools/ui_layout_preview.c 在 PC 上用【同一组数字】
  //  + 同一批字模渲染成 ASCII 图，改排版前后都能先看一眼再烧板子。
  //
  //  行盒预算（y 从 0 到 63）：
  //      标题行   0..13    基线 11
  //      大字号行 13..34   基线 29
  //      第三行   34..47   基线 45
  //      第四行   47..60   基线 58
  float rowTitleY = 11;
  float rowBigY   = 29;
  float row3Y     = 45;
  float row4Y     = 58;
};

static config &getUIConfig() {
  static config astraConfig;
  return astraConfig;
}
}
#endif //ASTRA_CORE_SRC_SYSTEM_H_