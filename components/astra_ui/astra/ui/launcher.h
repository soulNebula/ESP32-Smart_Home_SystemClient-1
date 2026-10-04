//
// Created by Fir on 2024/2/2.
//
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
  virtual ~Launcher() = default;  /* ESP32 移植：astraCoreDestroy 经基类指针 delete */

  void popInfo(std::string _info, uint16_t _time);

  void init(Menu* _rootPage);

  bool open();
  bool close();

  // ESP32 移植：页面内容动态变化（无人机列表重建）后由业务层调用。
  // 仅当 _page 为当前页时生效：钳位选中项、重注入选择框、摄像机复位。
  void refreshPage(Menu* _page);

  // ESP32 移植：业务层直接导航到指定页（日志页 OK → 热点信息页等
  // 按键驱动跳转）。目标页需已挂好 parent（getPreview 依赖 parent 返回上一页）。
  // 跳过 open() 的 getNext/空页检查——调用方保证目标页有效。
  bool openTarget(Menu* _page);

  void update();

  Camera* getCamera() { return camera; }
  Selector* getSelector() { return selector; }
};
}

#endif //ASTRA_CORE_SRC_ASTRA_UI_SCHEDULER_H_