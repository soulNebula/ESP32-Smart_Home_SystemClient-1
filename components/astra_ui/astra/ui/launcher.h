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
  // 删启动器走这里
  virtual ~Launcher() = default;

  void popInfo(std::string _info, uint16_t _time);

  void init(Menu* _rootPage);

  bool open();
  bool close();

  // 页面内容变了重画
  void refreshPage(Menu* _page);

  // 直接跳到指定页
  bool openTarget(Menu* _page);

  void update();

  Camera* getCamera() { return camera; }
  Selector* getSelector() { return selector; }
};
}

#endif