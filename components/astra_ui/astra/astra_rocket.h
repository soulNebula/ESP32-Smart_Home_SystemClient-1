/*
 * 模块：
 *   界面框架的页面表。声明主页、自检、设备、传感器、联动、关于
 *   这些页面类，向上给 astra_glue.cpp 调，向下用 launcher 和 hal。
 *
 * 功能：
 *   声明各页面类
 *   声明外部注入口
 *   声明页面指针
 */

#ifndef ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_
#define ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

/* 功能：界面开起停的接口 */
void astraCoreInit(void);
void astraCoreStart(void);
void astraCoreDestroy(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "../astra/ui/launcher.h"
#include "../hal/esp32/astra_hal_esp32.h"

extern astra::Launcher* astraLauncher;
extern astra::Menu* rootPage;

/* 功能：行数要和页面一致 */
#define UI_SELFTEST_ITEMS   9
#define UI_DEVICE_ITEMS     8
#define UI_AUTO_ITEMS       7

/* 功能：开机主页 */
class HomePage : public astra::Menu {
public:
  explicit HomePage(std::string _title);
  void render(std::vector<float> _camera) override;

  std::string tempHumiStr;   /* 功能：温湿度文字 */
  std::string lightRainStr;  /* 功能：光照雨滴文字 */
  std::string netStr;        /* 功能：网络状态文字 */
  std::string autoStr;       /* 功能：联动开关文字 */
};

/* 功能：带顶栏的列表页 */
class TitleListPage : public astra::Menu {
public:
  TitleListPage(std::string _title);
  TitleListPage(std::string _title, std::vector<uint8_t> _pic);
  void render(std::vector<float> _camera) override;

  std::string statusStr;   /* 功能：顶栏右侧小字 */
};

/* 功能：自检页 */
class SelfTestPage : public TitleListPage {
public:
  explicit SelfTestPage(std::string _title);
  bool onOkKey() override;   /* 功能：OK 交给自检处理 */
  void onEnter() override;   /* 功能：进页面就开自检 */
  void onExit() override;    /* 功能：离开就停自检 */
};

/* 功能：设备控制页 */
class DevicePage : public TitleListPage {
public:
  explicit DevicePage(std::string _title);
  bool onOkKey() override;
};

/* 功能：传感器大字页 */
class SensorPage : public astra::Menu {
public:
  explicit SensorPage(std::string _title);
  void render(std::vector<float> _camera) override;

  std::string tempStr;   /* 功能：温度文字 */
  std::string humiStr;   /* 功能：湿度文字 */
  std::string lightStr;  /* 功能：光照文字 */
  std::string rainStr;   /* 功能：雨滴文字 */
};

/* 功能：自动联动页 */
class AutoPage : public TitleListPage {
public:
  explicit AutoPage(std::string _title);
  bool onOkKey() override;   /* 功能：只第一行管开关 */
};

/* 功能：关于页 */
class AboutPage : public TitleListPage {
public:
  explicit AboutPage(std::string _title, std::vector<uint8_t> _pic);
};

/* 功能：页面指针给 glue 用 */
extern HomePage* g_pageHome;
extern SelfTestPage* g_pageSelfTest;
extern DevicePage* g_pageDevice;
extern SensorPage* g_pageSensor;
extern AutoPage* g_pageAuto;

/* 功能：glue 送来检测回调 */
void astraSelfTestSetOkCb(void (*cb)(int itemIndex));

/* 功能：glue 送来进出回调 */
void astraSelfTestSetEnterCb(void (*cb)(void));
void astraSelfTestSetExitCb(void (*cb)(void));

/* 功能：glue 送来开关回调 */
void astraDeviceSetOkCb(void (*cb)(int itemIndex));

/* 功能：glue 送来联动回调 */
void astraAutoSetOkCb(void (*cb)(void));

#endif

#endif
