/*
 * 模块：
 *   界面启动器。管页面的开和关、选择框和画面跟着动，被 astra_rocket.cpp
 *   起停并每帧调用，自己向下用 item.h 里的页面类。
 *
 * 功能：
 *   开选中页
 *   回上一页
 *   每帧刷画面
 */
#pragma once
#ifndef ASTRA_CORE_SRC_ASTRA_UI_SCHEDULER_H_
#define ASTRA_CORE_SRC_ASTRA_UI_SCHEDULER_H_

#include "element/page/item.h"

namespace astra {

class Launcher : public Animation {
private:
  Menu* currentPage;
  Selector* selector;
  Camera* camera;

  uint64_t time;

public:
  virtual ~Launcher() = default;  /* 功能：删启动器走这里 */

  void popInfo(std::string _info, uint16_t _time);

  void init(Menu* _rootPage);

  bool open();
  bool close();

  /* 功能：页面内容变了重画 */
  void refreshPage(Menu* _page);

  /* 功能：直接跳到指定页 */
  bool openTarget(Menu* _page);

  void update();

  Camera* getCamera() { return camera; }
  Selector* getSelector() { return selector; }
};
}

#endif