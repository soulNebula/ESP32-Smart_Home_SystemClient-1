/*
 * 模块：
 *   界面框架的页面层。搭出智能家居的整棵页面树（主页、自检、设备、
 *   传感器、联动、关于），向上被 astra_glue.cpp 调着刷文字，
 *   向下用页面类和 hal 画到屏上。
 *
 * 功能：
 *   建整棵页面树
 *   挂页面回调
 *   跑界面主循环
 */

#include <vector>
#include <utility>
#include <cstdio>
#include "astra_rocket.h"

astra::Launcher* astraLauncher = new astra::Launcher();
astra::Menu* rootPage = new astra::Menu("root");

HomePage*      g_pageHome     = nullptr;
SelfTestPage*  g_pageSelfTest = nullptr;
DevicePage*    g_pageDevice   = nullptr;
SensorPage*    g_pageSensor   = nullptr;
AutoPage*      g_pageAuto     = nullptr;

/* 功能：存 glue 送的回调 */

static void (*g_selftest_ok_cb)(int itemIndex) = nullptr;
static void (*g_device_ok_cb)(int itemIndex)   = nullptr;
static void (*g_auto_ok_cb)(void)              = nullptr;

void astraSelfTestSetOkCb(void (*cb)(int itemIndex)) { g_selftest_ok_cb = cb; }
void astraDeviceSetOkCb(void (*cb)(int itemIndex))   { g_device_ok_cb   = cb; }
void astraAutoSetOkCb(void (*cb)(void))              { g_auto_ok_cb     = cb; }

static void (*g_selftest_enter_cb)(void) = nullptr;
static void (*g_selftest_exit_cb)(void)  = nullptr;

void astraSelfTestSetEnterCb(void (*cb)(void)) { g_selftest_enter_cb = cb; }
void astraSelfTestSetExitCb(void (*cb)(void))  { g_selftest_exit_cb  = cb; }

/* 功能：五个磁贴的图标 */

static const uint8_t pic_selftest[120] = {
    0xFF, 0xFF, 0xFF, 0xFC,
    0xFF, 0xFF, 0xFF, 0xFC,
    0xC0, 0x00, 0x00, 0x0C,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xDC, 0x00, 0x00, 0xEC,
    0xDC, 0x00, 0x00, 0xEC,
    0xDC, 0x00, 0x06, 0xEC,
    0xDC, 0x00, 0x0E, 0xEC,
    0xDC, 0x00, 0x1C, 0xEC,
    0xDC, 0x00, 0x38, 0xEC,
    0xDC, 0x00, 0x70, 0xEC,
    0xDC, 0x00, 0xE0, 0xEC,
    0xDC, 0x00, 0xC0, 0xEC,
    0xDC, 0x61, 0xC0, 0xEC,
    0xDC, 0x63, 0x80, 0xEC,
    0xDC, 0x77, 0x00, 0xEC,
    0xDC, 0x3E, 0x00, 0xEC,
    0xDC, 0x1C, 0x00, 0xEC,
    0xDC, 0x0C, 0x00, 0xEC,
    0xDC, 0x00, 0x00, 0xEC,
    0xDC, 0x00, 0x00, 0xEC,
    0xDC, 0x00, 0x00, 0xEC,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xDF, 0xFF, 0xFF, 0xEC,
    0xC0, 0x00, 0x00, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFC,
    0xFF, 0xFF, 0xFF, 0xFC
};

static const uint8_t pic_device[120] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x03, 0xC0, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

static const uint8_t pic_sensor[120] = {
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x07, 0xC0, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x3F, 0xF8, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x0F, 0xE0, 0x00,
    0x00, 0x01, 0x00, 0x00
};

static const uint8_t pic_auto[120] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

static const uint8_t pic_about[120] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x01, 0xFF, 0xFF, 0x00,
    0x03, 0xFE, 0xFF, 0x80,
    0x03, 0xF0, 0x1F, 0x80,
    0x07, 0xE7, 0xCF, 0xC0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x0F, 0x87, 0xC3, 0xE0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x07, 0xC7, 0xC7, 0xC0,
    0x07, 0xE7, 0xCF, 0xC0,
    0x03, 0xF7, 0xDF, 0x80,
    0x03, 0xFF, 0xFF, 0x80,
    0x01, 0xFF, 0xFF, 0x00,
    0x00, 0xFF, 0xFE, 0x00,
    0x00, 0x7F, 0xFC, 0x00,
    0x00, 0x1F, 0xF0, 0x00,
    0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* 功能：把图标转成数组 */
static std::vector<uint8_t> pic(const uint8_t *p)
{
    return std::vector<uint8_t>(p, p + 120);
}

/* 功能：量这段字多宽 */

static float textWidth(const std::string &_text)
{
    std::string t = _text;   /* 功能：接口要非只读串 */
    return (float)HAL::getFontWidth(t);
}

/* 功能：造个开机主页 */
HomePage::HomePage(std::string _title) : astra::Menu(std::move(_title))
{
    hideSelector = true;               /* 功能：这页自己画 */
    openableWhenEmpty = true;
}

/* 功能：画主页四行文字 */
void HomePage::render(std::vector<float> _camera)
{
    (void)_camera;   /* 功能：这页不跟镜头动 */
    Item::updateConfig();
    HAL::setDrawType(1);

    /* 功能：四行从上往下排 */
    const astra::config &cfg = astra::getUIConfig();

    /* 功能：第一行画标题 */
    HAL::setFont(cfg.mainFont);
    HAL::drawChinese((systemConfig.screenWeight - textWidth(title)) / 2.0f, cfg.rowTitleY, title);
    if (!autoStr.empty()) {
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(autoStr), cfg.rowTitleY, autoStr);
    }

    /* 功能：第二行画温湿度 */
    HAL::setFont(u8g2_font_10x20_mn);
    const std::string &t = tempHumiStr.empty() ? "--.-C --%" : tempHumiStr;
    HAL::drawEnglish((systemConfig.screenWeight - textWidth(t)) / 2.0f, cfg.rowBigY, t);
    HAL::setFont(cfg.mainFont);   /* 功能：换回主字体 */

    /* 功能：第三行画光照 */
    HAL::drawChinese((systemConfig.screenWeight - textWidth(lightRainStr)) / 2.0f, cfg.row3Y,
                     lightRainStr);
    /* 功能：第四行画网络 */
    HAL::drawChinese((systemConfig.screenWeight - textWidth(netStr)) / 2.0f, cfg.row4Y,
                     netStr);
}

TitleListPage::TitleListPage(std::string _title) : astra::Menu(std::move(_title))
{
    clipTop = astra::getUIConfig().listLineHeight;   /* 功能：顶栏占一行 */
}

TitleListPage::TitleListPage(std::string _title, std::vector<uint8_t> _pic)
    : astra::Menu(std::move(_title), std::move(_pic))
{
    clipTop = astra::getUIConfig().listLineHeight;
}

/* 功能：画顶栏和列表 */
void TitleListPage::render(std::vector<float> _camera)
{
    if (child.empty()) {           /* 功能：空页面兜个底 */
        astra::Menu::render(_camera);
        return;
    }
    astra::Menu::render(_camera);
    Item::updateConfig();
    /* 功能：顶上画标题条 */
    HAL::setDrawType(1);
    HAL::drawBox(0, 0, systemConfig.screenWeight, astra::getUIConfig().listLineHeight);
    HAL::setDrawType(0);   /* 功能：白底上写黑字 */
    HAL::drawChinese((systemConfig.screenWeight - textWidth(title)) / 2.0f,
                     2 + HAL::getFontHeight(), title);
    if (!statusStr.empty()) {
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(statusStr),
                         2 + HAL::getFontHeight(), statusStr);
    }
    HAL::setDrawType(1);
}

SelfTestPage::SelfTestPage(std::string _title) : TitleListPage(std::move(_title))
{
}

/* 功能：把 OK 转给 glue */
bool SelfTestPage::onOkKey()
{
    if (g_selftest_ok_cb != nullptr) {
        g_selftest_ok_cb(selectIndex);
    }
    return true;   /* 功能：OK 这页自己管 */
}

/* 功能：进页面开自检 */
void SelfTestPage::onEnter()
{
    if (g_selftest_enter_cb != nullptr) {
        g_selftest_enter_cb();
    }
}

/* 功能：离开就停自检 */
void SelfTestPage::onExit()
{
    if (g_selftest_exit_cb != nullptr) {
        g_selftest_exit_cb();
    }
}

DevicePage::DevicePage(std::string _title) : TitleListPage(std::move(_title))
{
}

/* 功能：把 OK 转给 glue */
bool DevicePage::onOkKey()
{
    if (g_device_ok_cb != nullptr) {
        g_device_ok_cb(selectIndex);
    }
    return true;
}

SensorPage::SensorPage(std::string _title) : astra::Menu(std::move(_title))
{
    hideSelector = true;               /* 功能：这页自己画 */
    openableWhenEmpty = true;
}

/* 功能：画四行文字 */
void SensorPage::render(std::vector<float> _camera)
{
    (void)_camera;
    Item::updateConfig();
    HAL::setDrawType(1);

    /* 功能：四行从上往下排 */
    const astra::config &cfg = astra::getUIConfig();

    /* 功能：第一行画标题 */
    HAL::setFont(cfg.mainFont);
    HAL::drawChinese((systemConfig.screenWeight - textWidth(title)) / 2.0f, cfg.rowTitleY, title);
    {
        const std::string hint = "返回";
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(hint), cfg.rowTitleY, hint);
    }

    /* 功能：中文字和数字分开画 */
    const std::string t = tempStr.empty() ? std::string("温度 --.-C") : tempStr;
    const size_t sp = t.find(' ');
    const std::string label = (sp == std::string::npos) ? std::string("温度") : t.substr(0, sp);
    const std::string value = (sp == std::string::npos) ? std::string("--.-C") : t.substr(sp + 1);

    HAL::drawChinese(2, cfg.rowBigY, label);
    const float labelW = textWidth(label);
    HAL::setFont(u8g2_font_10x20_mn);
    HAL::drawEnglish(2 + labelW + 2, cfg.rowBigY, value);
    HAL::setFont(cfg.mainFont);

    /* 功能：第三行画湿度 */
    HAL::drawChinese(2, cfg.row3Y, humiStr);
    /* 功能：第四行左右各一段 */
    HAL::drawChinese(2, cfg.row4Y, lightStr);
    if (!rainStr.empty()) {
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(rainStr), cfg.row4Y, rainStr);
    }
}

AutoPage::AutoPage(std::string _title) : TitleListPage(std::move(_title))
{
}

/* 功能：切联动总开关 */
bool AutoPage::onOkKey()
{
    if (selectIndex == 0 && g_auto_ok_cb != nullptr) {
        g_auto_ok_cb();    /* 功能：只有第一行响应 */
    }
    return true;           /* 功能：别行按了也白按 */
}

AboutPage::AboutPage(std::string _title, std::vector<uint8_t> _pic)
    : TitleListPage(std::move(_title), std::move(_pic))
{
}

/* 功能：把界面搭起来 */
void astraCoreInit(void) {
  HAL::inject(new AstraHALEsp32);

  /* 功能：先建主页 */
  g_pageHome = new HomePage("智能家居");

  /* 功能：建五个磁贴 */
  rootPage->addItem(new astra::Menu("自检", pic(pic_selftest)));
  rootPage->addItem(new astra::Menu("设备控制", pic(pic_device)));
  rootPage->addItem(new astra::Menu("传感器", pic(pic_sensor)));
  rootPage->addItem(new astra::Menu("自动联动", pic(pic_auto)));
  rootPage->addItem(new astra::Menu("关于", pic(pic_about)));
  g_pageHome->addItem(rootPage);   /* 功能：主页挂菜单 */

  /* 功能：建自检页 */
  g_pageSelfTest = new SelfTestPage("自检");
  for (int i = 0; i < UI_SELFTEST_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageSelfTest->addItem(row);
  }
  rootPage->child[0]->addItem(g_pageSelfTest);

  /* 功能：建设备页 */
  g_pageDevice = new DevicePage("设备控制");
  for (int i = 0; i < UI_DEVICE_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageDevice->addItem(row);
  }
  rootPage->child[1]->addItem(g_pageDevice);

  /* 功能：建传感器页 */
  g_pageSensor = new SensorPage("传感器");
  rootPage->child[2]->addItem(g_pageSensor);

  /* 功能：建联动页 */
  g_pageAuto = new AutoPage("自动联动");
  for (int i = 0; i < UI_AUTO_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageAuto->addItem(row);
  }
  rootPage->child[3]->addItem(g_pageAuto);

  /* 功能：建关于页 */
  auto *pageAbout = new AboutPage("关于", pic(pic_about));
  pageAbout->addItem(new astra::Menu("ESP32-S3 智能家居"));
  pageAbout->addItem(new astra::Menu("语音/按键/MQTT/BLE"));
  pageAbout->addItem(new astra::Menu("自动联动+五键自检"));
  pageAbout->addItem(new astra::Menu("OK 执行 LEFT 返回"));
  rootPage->child[4]->addItem(pageAbout);

  /* 功能：把首页交给启动器 */
  astraLauncher->init(g_pageHome);
}

/* 功能：界面一直转 */
void astraCoreStart(void) {
  for (;;) {
    astraLauncher->update();
  }
}

/* 功能：拆掉界面 */
void astraCoreDestroy(void) {
  HAL::destroy();
  delete astraLauncher;
}
