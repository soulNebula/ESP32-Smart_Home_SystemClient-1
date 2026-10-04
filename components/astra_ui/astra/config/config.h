/*
 * 模块：
 *   界面参数表。屏幕多大、每行摆在哪、动画跑多快都记在这里，
 *   被整个界面层读取；谁要画东西先来这里取值。
 *
 * 功能：
 *   定屏幕尺寸
 *   定每行位置
 *   定动画速度
 */

#pragma once
#ifndef ASTRA_CORE_SRC_SYSTEM_H_
#define ASTRA_CORE_SRC_SYSTEM_H_

#include "cstdint"
#include "u8g2.h"

namespace astra {
/* 功能：界面参数打包 */
struct config {
  /* 功能：帧率低就调快动画 */
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
  float listTextMargin = 4; /* 功能：文字边距 */
  float listLineHeight = 16;
  float selectorRadius = 0.5f;
  float selectorMargin = 4; /* 功能：框离文字多远 */
  float selectorTopMargin = 2; /* 功能：框比文字高多少 */

  uint8_t listPageTurningMode = 1; /* 功能：0 翻页 1 滚动 */

  float tilePicWidth = 30;
  float tilePicHeight = 30;
  float tilePicMargin = 8;
  float tilePicTopMargin = 8; /* 功能：图标上边距 */
  float tileArrowWidth = 6;
  float tileArrowMargin = 4; /* 功能：箭头边距 */

  /* 功能：这三个数值要配套改 */
  float tileDottedLineBottomMargin = 18; /* 功能：虚线的下边留白 */
  float tileArrowBottomMargin = 8; /* 功能：箭头的下边留白 */
  float tileTextBottomMargin = 12; /* 功能：标题的下边留白 */

  float tileBarHeight = 2; /* 功能：进度条多高 */

  float tileSelectBoxLineLength = 5;  /* 功能：选择框的角多长 */
  float tileSelectBoxMargin = 3; /* 功能：选择框边距 */
  float tileSelectBoxWidth = tileSelectBoxMargin * 2 + tilePicWidth; /* 功能：选择框多宽 */
  float tileSelectBoxHeight = tileSelectBoxMargin * 2 + tilePicHeight; /* 功能：选择框多高 */
  float tileTitleHeight = 8; /* 功能：磁贴标题多高 */

  float tileBtnMargin = 16; /* 功能：按钮边距 */

  float popMargin = 4; /* 功能：弹窗边距 */
  float popRadius = 2; /* 功能：弹窗圆角多大 */
  float popSpeed = 90; /* 功能：弹窗动画快慢 */

  float logoStarLength = 2; /* 功能：星星的线多长 */
  float logoTextHeight = 14; /* 功能：logo 文字多高 */
  float logoCopyRightHeight = 8; /* 功能：版权字多高 */
  uint8_t logoStarNum = 16; /* 功能：星星画几颗 */
  /* 功能：换成能显中文的字体 */
  const uint8_t *logoTitleFont = u8g2_font_wqy12_t_gb2312;
  const uint8_t *logoCopyRightFont = u8g2_font_6x10_tf;

  const uint8_t *mainFont = u8g2_font_wqy12_t_gb2312;

  /* 功能：四行文字的基线 */
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
#endif