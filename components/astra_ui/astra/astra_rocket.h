#ifndef ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_
#define ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

// 界面开起停的接口
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

// 行数要和页面一致
#define UI_SELFTEST_ITEMS   9
#define UI_DEVICE_ITEMS     8
#define UI_AUTO_ITEMS       7

// 开机主页
class HomePage : public astra::Menu {
public:
  explicit HomePage(std::string _title);
  void render(std::vector<float> _camera) override;

  // 温湿度文字
  std::string tempHumiStr;
  // 光照雨滴文字
  std::string lightRainStr;
  // 网络状态文字
  std::string netStr;
  // 联动开关文字
  std::string autoStr;
};

// 带顶栏的列表页
class TitleListPage : public astra::Menu {
public:
  TitleListPage(std::string _title);
  TitleListPage(std::string _title, std::vector<uint8_t> _pic);
  void render(std::vector<float> _camera) override;

  // 顶栏右侧小字
  std::string statusStr;
};

// 自检页
class SelfTestPage : public TitleListPage {
public:
  explicit SelfTestPage(std::string _title);
  // OK 交给自检处理
  bool onOkKey() override;
  // 进页面就开自检
  void onEnter() override;
  // 离开就停自检
  void onExit() override;
};

// 设备控制页
class DevicePage : public TitleListPage {
public:
  explicit DevicePage(std::string _title);
  bool onOkKey() override;
};

// 传感器大字页
class SensorPage : public astra::Menu {
public:
  explicit SensorPage(std::string _title);
  void render(std::vector<float> _camera) override;

  // 温度文字
  std::string tempStr;
  // 湿度文字
  std::string humiStr;
  // 光照文字
  std::string lightStr;
  // 雨滴文字
  std::string rainStr;
};

// 自动联动页
class AutoPage : public TitleListPage {
public:
  explicit AutoPage(std::string _title);
  // 只第一行管开关
  bool onOkKey() override;
};

// 关于页
class AboutPage : public TitleListPage {
public:
  explicit AboutPage(std::string _title, std::vector<uint8_t> _pic);
};

// 页面指针给 glue 用
extern HomePage* g_pageHome;
extern SelfTestPage* g_pageSelfTest;
extern DevicePage* g_pageDevice;
extern SensorPage* g_pageSensor;
extern AutoPage* g_pageAuto;

// glue 送来检测回调
void astraSelfTestSetOkCb(void (*cb)(int itemIndex));

// glue 送来进出回调
void astraSelfTestSetEnterCb(void (*cb)(void));
void astraSelfTestSetExitCb(void (*cb)(void));

// glue 送来开关回调
void astraDeviceSetOkCb(void (*cb)(int itemIndex));

// glue 送来联动回调
void astraAutoSetOkCb(void (*cb)(void));

#endif

#endif
