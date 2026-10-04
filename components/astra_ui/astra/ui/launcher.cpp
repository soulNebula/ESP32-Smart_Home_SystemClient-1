//
// Created by Fir on 2024/2/2.
//

#include "launcher.h"
#include "esp_log.h"   /* 诊断埋点：导航/弹窗行为上串口 */

namespace astra {

static const char *TAG_L = "astra_launcher";

void Launcher::popInfo(std::string _info, uint16_t _time) {
  //ESP32 移植修复：原实现用 static 局部变量缓存 beginTime/wPop 等，
  //导致第二次调用起全部失效（static 初始化只执行一次）。改为调用期局部变量。
  //ESP32 移植修复 2（2026-10-02）：原实现的"时长"是【渲染循环计数】而非毫秒
  //（每轮一次整屏 canvasUpdate 约 6ms，1200 次 ≈ 7.2 秒），弹窗赖在屏上不走。
  //改用 HAL::millis() 真实时间：_time 就是毫秒。
  const unsigned long beginMs = HAL::millis();
  ESP_LOGI(TAG_L, "popInfo 开始：%s（%u ms）", _info.c_str(), (unsigned)_time);

  float wPop = HAL::getFontWidth(_info) + 2 * getUIConfig().popMargin;  //宽度
  float hPop = HAL::getFontHeight() + 2 * getUIConfig().popMargin;  //高度

  float yPop = 0 - hPop - 8; //从屏幕上方滑入
  float yPopTrg = 0;

  float xPop = (HAL::getSystemConfig().screenWeight - wPop) / 2;  //居中

  while (true) {
    time++;

    if (HAL::millis() - beginMs < _time) yPopTrg = (HAL::getSystemConfig().screenHeight - hPop) / 3;  //目标位置 中间偏上
    else yPopTrg = 0 - hPop - 8;  //滑出

    HAL::canvasClear();
    /*渲染一帧*/
    currentPage->render(camera->getPosition());
    selector->render(camera->getPosition());
    camera->update(currentPage, selector);
    /*渲染一帧*/

    HAL::setDrawType(0);
    HAL::drawRBox(xPop - 4, yPop - 4, wPop + 8, hPop + 8, getUIConfig().popRadius + 2);
    HAL::setDrawType(1);  //反色显示
    HAL::drawRFrame(xPop - 1, yPop - 1, wPop + 2, hPop + 2, getUIConfig().popRadius);  //绘制一个圆角矩形
    /* ESP32 移植：原 drawEnglish 走 u8g2_DrawStr（只支持 ASCII），中文弹窗
     * （语音助手提示等）会乱码。drawChinese 走 u8g2_DrawUTF8，中英文都支持。 */
    HAL::drawChinese(xPop + getUIConfig().popMargin, yPop + getUIConfig().popMargin + HAL::getFontHeight(), _info);  //绘制文字

    HAL::canvasUpdate();

    animation(&yPop, yPopTrg, getUIConfig().popSpeed);  //动画

    if (HAL::millis() - beginMs >= _time && yPop == 0 - hPop - 8) break;  //退出条件（真实毫秒）
    if (HAL::getAnyKey()) break;  //按键打断弹出
  }
  ESP_LOGI(TAG_L, "popInfo 结束：%s", _info.c_str());
}

void Launcher::init(Menu *_rootPage) {
  currentPage = _rootPage;

  camera = new Camera(0, 0);
  _rootPage->init(camera->getPosition());

  selector = new Selector();
  selector->inject(_rootPage);
  selector->go(_rootPage->selectIndex);

  //open();
}

/**
 * @brief 打开选中的页面
 *
 * @return 是否成功打开
 * @warning 仅可调用一次
 */
bool Launcher::open() {
  //todo 打开和关闭都还没写完 应该还漏掉了一部分内容

  //如果当前页面指向的当前item没有后继 那就返回false
  if (currentPage->getNext() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  // ESP32 移植：目标页为 0 子项默认拒绝——叶子行（详情页的行项等）
  // 无子页可打开，进入后 Menu::render 对 getItemNum()=0 除零 →
  // inf/NaN 坐标绘制死循环 → 看门狗复位（快速按键误触的主要崩溃源）。
  // 空态页面（0 架时的无人机列表，自带空态渲染）经 openableWhenEmpty 豁免
  if (currentPage->getNext()->getItemNum() == 0 && !currentPage->getNext()->openableWhenEmpty) {
    popInfo("empty page!", 600);
    return false;
  }

  currentPage->onExit();  // ESP32 移植：页面级离开回调（蓝牙遥测页关 BLE 外设等）
  currentPage->deInit();  //先析构（退场动画）再挪动指针

  currentPage = currentPage->getNext();
  camera->reset();  // ESP32 移植：每个页面从初始位置（顶栏下第一行）打开
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  //selector->go(currentPage->selectIndex);
  currentPage->onEnter();  // ESP32 移植：页面级进入回调（蓝牙遥测页开 BLE 外设等）

  ESP_LOGI(TAG_L, "open → %s", currentPage->title.c_str());
  return true;
}

/**
 * @brief 关闭选中的页面
 *
 * @return 是否成功关闭
 * @warning 仅可调用一次
 */
bool Launcher::close() {
  if (currentPage->getPreview() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  // ESP32 移植：同样允许返回空页（见 open() 注释）

  currentPage->onExit();  // ESP32 移植：页面级离开回调（蓝牙遥测页关 BLE 外设等）
  currentPage->deInit();  //先析构（退场动画）再挪动指针

  currentPage = currentPage->getPreview();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  //selector->go(currentPage->selectIndex);
  currentPage->onEnter();  // ESP32 移植：页面级进入回调

  ESP_LOGI(TAG_L, "close ← %s", currentPage->title.c_str());
  return true;
}

void Launcher::refreshPage(Menu* _page) {
  // ESP32 移植：页面内容动态变化（无人机列表重建）后由业务层调用。
  // 仅当 _page 为当前页时生效：钳位选中项、重注入选择框、摄像机复位。
  if (_page == nullptr || _page != currentPage) return;

  if (_page->getItemNum() > 0) {
    if (_page->selectIndex >= _page->getItemNum()) _page->selectIndex = 0;
    selector->inject(_page);
  } else {
    selector->inject(_page);  // 空页：仅接管指针，render 不绘制选择框
  }
  camera->reset();  // 列表缩短/重排后视角回顶，避免停留在无效滚动位置
}

bool Launcher::openTarget(Menu* _page) {
  // ESP32 移植：业务层直接导航（日志页 OK → 热点信息页）。
  // 目标页不在当前页 child 列表内也可跳转，返回路径靠目标页的 parent。
  if (_page == nullptr || _page == currentPage) return false;

  currentPage->onExit();  // ESP32 移植：页面级离开回调（蓝牙遥测页关 BLE 外设等）
  currentPage->deInit();  //先析构（退场动画）再挪动指针

  currentPage = _page;
  camera->reset();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  currentPage->onEnter();  // ESP32 移植：页面级进入回调

  return true;
}

void Launcher::update() {
  HAL::canvasClear();

  currentPage->render(camera->getPosition());
  selector->render(camera->getPosition());
  camera->update(currentPage, selector);

  /* ESP32 移植：替换原时间轴测试代码为真实按键处理。
   * UP/DOWN 移动选择（menuLoop 回绕），OK 打开选中页，LEFT 返回上一页。 */
  for (int i = 0; i < key::KEY_NUM; i++) {
    key::keyAction act = HAL::getKeyAction(static_cast<key::KEY_INDEX>(i));
    if (act != key::CLICK) continue;

    uint8_t itemNum = currentPage->getItemNum();
    if (i == key::KEY_UP) {
      if (itemNum > 0) {
        selector->go((currentPage->selectIndex + itemNum - 1) % itemNum);
      }
    } else if (i == key::KEY_DOWN) {
      if (itemNum > 0) {
        selector->go((currentPage->selectIndex + 1) % itemNum);
      }
    } else if (i == key::KEY_OK) {
      // ESP32 移植：先问页面是否自行处理 OK（日志页开关热点/热点信息页
      // 退出等），处理了则不导航。空页（0 子项）按 OK 会经 getNext() 对
      // 空 vector 取 child[selectIndex] → NULL 解引用崩溃，保留 itemNum 防护。
      // 默认 onOkKey()=false → 所有既有页面行为不变
      if (!currentPage->onOkKey() && itemNum > 0) open();
    } else if (i == key::KEY_LEFT) {
      // ESP32 移植：先问页面是否自行处理 LEFT（统计页锁定占比行编辑态加减），
      // 处理了则不返回上一页。默认 onLeftKey()=false → 既有页面行为不变
      if (!currentPage->onLeftKey()) close();
    } else if (i == key::KEY_RIGHT) {
      // ESP32 移植：RIGHT 此前未使用；默认 onRightKey()=false 空实现 → 行为不变
      currentPage->onRightKey();
    }
  }
  HAL::clearKeyActions();

  HAL::canvasUpdate();

  time++;
}
}