//
// astra_rocket.cpp
// ESP32-S3 智能家居：astra UI 菜单树（页面层，替换 RID 接收机业务）。
// 页面结构见 astra_rocket.h 顶部说明。字段刷新由 main 组件的 astra_glue.cpp
// 每帧完成（组件不依赖 main，靠回调注入，依赖方向单向）。
//

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

/* ---------- 注入回调（glue 设置） ---------- */

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

/* ---------- 磁贴图标（30×30 XBMP） ---------- */

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

static std::vector<uint8_t> pic(const uint8_t *p)
{
    return std::vector<uint8_t>(p, p + 120);
}

/* ---------- 工具 ---------- */

static float textWidth(const std::string &_text)
{
    std::string t = _text;   /* HAL::getFontWidth 签名要求非 const 引用 */
    return (float)HAL::getFontWidth(t);
}

/* ========================================================================= */
/*  页面实现                                                                */
/* ========================================================================= */

HomePage::HomePage(std::string _title) : astra::Menu(std::move(_title))
{
    hideSelector = true;               /* 自定义渲染，无选择框 */
    openableWhenEmpty = true;
}

void HomePage::render(std::vector<float> _camera)
{
    (void)_camera;   /* 主页面固定布局，不随摄像机移动 */
    Item::updateConfig();
    HAL::setDrawType(1);

    /* ★ 排版（2026-09-28 修）：原来把后三行画在 y=49 / 58 / 62，基线只差 4~9px，
     *   而中文行高约 13px → 三行叠成一团，"除温湿度外全看不清"。
     *   现在压成 4 行，行基线统一取自 astra::config（见 config.h 的行盒预算）：
     *       标题(含右侧自动联动状态) / 温湿度大字号 / 光照雨滴 / 网络状态
     *   原来的"OK 菜单"提示占了一行，已去掉 —— 64px 放不下 5 行中文。 */
    const astra::config &cfg = astra::getUIConfig();

    /* 第 1 行：标题居中 + 右侧自动联动状态 */
    HAL::setFont(cfg.mainFont);
    HAL::drawChinese((systemConfig.screenWeight - textWidth(title)) / 2.0f, cfg.rowTitleY, title);
    if (!autoStr.empty()) {
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(autoStr), cfg.rowTitleY, autoStr);
    }

    /* 第 2 行：温湿度大字号（10x20_mn 数字字体） */
    HAL::setFont(u8g2_font_10x20_mn);
    const std::string &t = tempHumiStr.empty() ? "--.-C --%" : tempHumiStr;
    HAL::drawEnglish((systemConfig.screenWeight - textWidth(t)) / 2.0f, cfg.rowBigY, t);
    HAL::setFont(cfg.mainFont);   /* 恢复主字体 */

    /* 第 3 行：光照 / 雨滴 */
    HAL::drawChinese((systemConfig.screenWeight - textWidth(lightRainStr)) / 2.0f, cfg.row3Y,
                     lightRainStr);
    /* 第 4 行：网络状态 */
    HAL::drawChinese((systemConfig.screenWeight - textWidth(netStr)) / 2.0f, cfg.row4Y,
                     netStr);
}

TitleListPage::TitleListPage(std::string _title) : astra::Menu(std::move(_title))
{
    clipTop = astra::getUIConfig().listLineHeight;   /* 顶栏占一行 */
}

TitleListPage::TitleListPage(std::string _title, std::vector<uint8_t> _pic)
    : astra::Menu(std::move(_title), std::move(_pic))
{
    clipTop = astra::getUIConfig().listLineHeight;
}

void TitleListPage::render(std::vector<float> _camera)
{
    if (child.empty()) {           /* 防御：实际页面总有行 */
        astra::Menu::render(_camera);
        return;
    }
    astra::Menu::render(_camera);
    Item::updateConfig();
    /* 顶栏：白底黑字实色条，标题居中；右侧可选状态小字 */
    HAL::setDrawType(1);
    HAL::drawBox(0, 0, systemConfig.screenWeight, astra::getUIConfig().listLineHeight);
    HAL::setDrawType(0);   /* 0=清空像素：在白底上画黑字 */
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

bool SelfTestPage::onOkKey()
{
    if (g_selftest_ok_cb != nullptr) {
        g_selftest_ok_cb(selectIndex);
    }
    return true;   /* OK 由页面处理，不执行 open() 导航 */
}

void SelfTestPage::onEnter()
{
    if (g_selftest_enter_cb != nullptr) {
        g_selftest_enter_cb();
    }
}

void SelfTestPage::onExit()
{
    if (g_selftest_exit_cb != nullptr) {
        g_selftest_exit_cb();
    }
}

DevicePage::DevicePage(std::string _title) : TitleListPage(std::move(_title))
{
}

bool DevicePage::onOkKey()
{
    if (g_device_ok_cb != nullptr) {
        g_device_ok_cb(selectIndex);
    }
    return true;
}

SensorPage::SensorPage(std::string _title) : astra::Menu(std::move(_title))
{
    hideSelector = true;               /* 自定义渲染，无选择框 */
    openableWhenEmpty = true;
}

void SensorPage::render(std::vector<float> _camera)
{
    (void)_camera;
    Item::updateConfig();
    HAL::setDrawType(1);

    /* 与主页共用同一套行基线（见 config.h 的"行盒预算"注释）。
     * 原来这里是 y=34/46/56/63 再加一个 y=63 的提示 —— 后三行只差 7~10px、
     * 提示还压在雨滴那行上，同样看不清。 */
    const astra::config &cfg = astra::getUIConfig();

    /* 第 1 行：标题居中 + 右侧"返回"提示（提示挪上来，给下面腾行盒） */
    HAL::setFont(cfg.mainFont);
    HAL::drawChinese((systemConfig.screenWeight - textWidth(title)) / 2.0f, cfg.rowTitleY, title);
    {
        const std::string hint = "返回";
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(hint), cfg.rowTitleY, hint);
    }

    /* 第 2 行：温度 —— 中文标签用 wqy12、数值用 10x20 大字号。
     * ★ 10x20_mn 里没有中文字形，整串丢给它只会把"温度"画成缺字。 */
    const std::string t = tempStr.empty() ? std::string("温度 --.-C") : tempStr;
    const size_t sp = t.find(' ');
    const std::string label = (sp == std::string::npos) ? std::string("温度") : t.substr(0, sp);
    const std::string value = (sp == std::string::npos) ? std::string("--.-C") : t.substr(sp + 1);

    HAL::drawChinese(2, cfg.rowBigY, label);
    const float labelW = textWidth(label);
    HAL::setFont(u8g2_font_10x20_mn);
    HAL::drawEnglish(2 + labelW + 2, cfg.rowBigY, value);
    HAL::setFont(cfg.mainFont);

    /* 第 3 行：湿度 */
    HAL::drawChinese(2, cfg.row3Y, humiStr);
    /* 第 4 行：光照（左） + 雨滴（右），同一个行盒里左右两段 */
    HAL::drawChinese(2, cfg.row4Y, lightStr);
    if (!rainStr.empty()) {
        HAL::drawChinese(systemConfig.screenWeight - 2 - textWidth(rainStr), cfg.row4Y, rainStr);
    }
}

AutoPage::AutoPage(std::string _title) : TitleListPage(std::move(_title))
{
}

bool AutoPage::onOkKey()
{
    if (selectIndex == 0 && g_auto_ok_cb != nullptr) {
        g_auto_ok_cb();    /* 仅第 0 行（联动总开关）响应 OK */
    }
    return true;           /* 其余行 OK 无动作 */
}

AboutPage::AboutPage(std::string _title, std::vector<uint8_t> _pic)
    : TitleListPage(std::move(_title), std::move(_pic))
{
}

/* ========================================================================= */
/*  C 入口                                                                  */
/* ========================================================================= */

void astraCoreInit(void) {
  HAL::inject(new AstraHALEsp32);

  /* 主页面（开机页）：温湿度概览 + 网络状态，OK 进入磁贴菜单页 */
  g_pageHome = new HomePage("智能家居");

  /* 磁贴菜单：5 个磁贴 */
  rootPage->addItem(new astra::Menu("自检", pic(pic_selftest)));
  rootPage->addItem(new astra::Menu("设备控制", pic(pic_device)));
  rootPage->addItem(new astra::Menu("传感器", pic(pic_sensor)));
  rootPage->addItem(new astra::Menu("自动联动", pic(pic_auto)));
  rootPage->addItem(new astra::Menu("关于", pic(pic_about)));
  g_pageHome->addItem(rootPage);   /* 主页 → OK → 菜单；菜单 LEFT → 返回主页 */

  /* 自检页：9 个测试项行（行标题由 glue 每帧刷新，当前项加 "*"） */
  g_pageSelfTest = new SelfTestPage("自检");
  for (int i = 0; i < UI_SELFTEST_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageSelfTest->addItem(row);
  }
  rootPage->child[0]->addItem(g_pageSelfTest);

  /* 设备控制页：8 行设备（"客厅灯 [开]" 等，glue 每帧刷新） */
  g_pageDevice = new DevicePage("设备控制");
  for (int i = 0; i < UI_DEVICE_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageDevice->addItem(row);
  }
  rootPage->child[1]->addItem(g_pageDevice);

  /* 传感器页：大字号温湿度/光照/雨滴 */
  g_pageSensor = new SensorPage("传感器");
  rootPage->child[2]->addItem(g_pageSensor);

  /* 自动联动页：第 0 行总开关（OK 切换），1~6 行阈值只读 */
  g_pageAuto = new AutoPage("自动联动");
  for (int i = 0; i < UI_AUTO_ITEMS; i++) {
    auto *row = new astra::Menu("-");
    g_pageAuto->addItem(row);
  }
  rootPage->child[3]->addItem(g_pageAuto);

  /* 关于页 */
  auto *pageAbout = new AboutPage("关于", pic(pic_about));
  pageAbout->addItem(new astra::Menu("ESP32-S3 智能家居"));
  pageAbout->addItem(new astra::Menu("语音/按键/MQTT/BLE"));
  pageAbout->addItem(new astra::Menu("自动联动+五键自检"));
  pageAbout->addItem(new astra::Menu("OK 执行 LEFT 返回"));
  rootPage->child[4]->addItem(pageAbout);

  /* ★ 调度器挂载根页面（漏了这行 Launcher::update() 会崩） */
  astraLauncher->init(g_pageHome);
}

void astraCoreStart(void) {
  for (;;) {  //NOLINT
    astraLauncher->update();
  }
}

void astraCoreDestroy(void) {
  HAL::destroy();
  delete astraLauncher;
}
