//
// astra_rocket.h
// ESP32-S3 智能家居：astra UI 页面层（替换 RID 接收机业务页面）。
// 页面结构：
//   主页面 HomePage（传感器/网络概览，OK 进菜单）
//     └─ 磁贴菜单 rootPage：自检 / 设备控制 / 传感器 / 自动联动 / 关于
//          ├─ SelfTestPage：9 个测试项列表，OK 执行（onOkKey 拦截）
//          ├─ DevicePage：8 个设备列表，OK 开关（onOkKey 拦截）
//          ├─ SensorPage：温湿度/光照/雨滴大字号显示（自定义渲染）
//          ├─ AutoPage：联动总开关（OK 切换）+ 阈值只读显示
//          └─ AboutPage：项目信息
// 全部字段（行标题/状态行）由 main 组件的 astra_glue.cpp 每帧刷新，
// 组件不依赖 main（依赖方向单向）。
//

#ifndef ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_
#define ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

/*---- C ----*/

void astraCoreInit(void);
void astraCoreStart(void);
void astraCoreDestroy(void);

/*---- C ----*/

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include "../astra/ui/launcher.h"
#include "../hal/esp32/astra_hal_esp32.h"

extern astra::Launcher* astraLauncher;
extern astra::Menu* rootPage;

/* 菜单行数常量（与页面 addItem 数量保持一致） */
#define UI_SELFTEST_ITEMS   9
#define UI_DEVICE_ITEMS     8
#define UI_AUTO_ITEMS       7

/**
 * @brief 主页面（开机页）：温湿度大字号 + 光照/雨滴 + 网络状态 + "OK 菜单"。
 *        自定义渲染（hideSelector），字段由 glue 每帧刷新。
 */
class HomePage : public astra::Menu {
public:
  explicit HomePage(std::string _title);
  void render(std::vector<float> _camera) override;

  std::string tempHumiStr;   /* "25.3C 52%"（无效时 "T:-- H:--"） */
  std::string lightRainStr;  /* "光照 65% 雨滴 6%" */
  std::string netStr;        /* "WiFi OK  MQTT OK" / "网络断开" */
  std::string autoStr;       /* "自动联动 开/关" */
};

/**
 * @brief 带顶部标题栏的列表页：标题居中显示在顶栏，下方分隔线，
 *        子项行整体下移一个行高（listLineHeight）。第二个构造带状态行
 *        statusStr（顶栏右侧小字，glue 每帧刷新，如自检页的"检测中/完成"）。
 */
class TitleListPage : public astra::Menu {
public:
  TitleListPage(std::string _title);
  TitleListPage(std::string _title, std::vector<uint8_t> _pic);
  void render(std::vector<float> _camera) override;

  std::string statusStr;   /* 顶栏右侧状态小字（可为空） */
};

/**
 * @brief 自检页：9 行测试项（行标题由 glue 每帧刷新，当前项加 "*"），
 *        顶栏状态 = 等待/检测中/完成。OK 由 onOkKey 拦截 → 注入回调执行测试。
 */
class SelfTestPage : public TitleListPage {
public:
  explicit SelfTestPage(std::string _title);
  bool onOkKey() override;   /* 调注入回调后返回 true：OK 由页面处理，不导航 */
  void onEnter() override;   /* 打开页面 → 注入回调启动自检状态机 */
  void onExit() override;    /* 离开页面 → 注入回调退出自检状态机 */
};

/**
 * @brief 设备控制页：8 行设备（"客厅灯 [开]" 等，glue 每帧刷新），
 *        OK 切换开关（onOkKey 拦截 → 注入回调）。
 */
class DevicePage : public TitleListPage {
public:
  explicit DevicePage(std::string _title);
  bool onOkKey() override;
};

/**
 * @brief 传感器页：温湿度/光照/雨滴 4 行大字号（自定义渲染，hideSelector），
 *        字段由 glue 每帧刷新；LEFT 返回上一页。
 */
class SensorPage : public astra::Menu {
public:
  explicit SensorPage(std::string _title);
  void render(std::vector<float> _camera) override;

  std::string tempStr;   /* "温度 25.3C" */
  std::string humiStr;   /* "湿度 52%" */
  std::string lightStr;  /* "光照 65%" */
  std::string rainStr;   /* "雨滴 6%" */
};

/**
 * @brief 自动联动页：第 0 行 = 联动总开关（OK 切换），第 1~6 行 = 阈值只读。
 *        行标题由 glue 每帧刷新。
 */
class AutoPage : public TitleListPage {
public:
  explicit AutoPage(std::string _title);
  bool onOkKey() override;   /* 仅选中第 0 行时切换联动开关 */
};

/**
 * @brief 关于页：项目名/版本/硬件/按键说明（静态）。
 */
class AboutPage : public TitleListPage {
public:
  explicit AboutPage(std::string _title, std::vector<uint8_t> _pic);
};

/* 页面句柄（astra_glue.cpp 每帧刷新动态标题） */
extern HomePage* g_pageHome;
extern SelfTestPage* g_pageSelfTest;
extern DevicePage* g_pageDevice;
extern SensorPage* g_pageSensor;
extern AutoPage* g_pageAuto;

/** glue 注入自检页 OK 回调（执行当前选中测试项）；NULL 时按 OK 无动作 */
void astraSelfTestSetOkCb(void (*cb)(int itemIndex));

/** glue 注入自检页进入/退出回调（打开页面启动自检状态机，离开时退出） */
void astraSelfTestSetEnterCb(void (*cb)(void));
void astraSelfTestSetExitCb(void (*cb)(void));

/** glue 注入设备页 OK 回调（切换设备开关）；NULL 时按 OK 无动作 */
void astraDeviceSetOkCb(void (*cb)(int itemIndex));

/** glue 注入自动联动页 OK 回调（切换联动总开关）；NULL 时按 OK 无动作 */
void astraAutoSetOkCb(void (*cb)(void));

#endif /* __cplusplus */

#endif  // ASTRA_CORE_SRC_ASTRA_ASTRA_ROCKET_H_
