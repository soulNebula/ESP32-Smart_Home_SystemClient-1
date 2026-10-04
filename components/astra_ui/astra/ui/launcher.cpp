/*
 * 模块：
 *   界面启动器。负责开页、返回、每帧重画，还管弹窗提示；被
 *   astra_rocket.cpp 的界面主循环每帧调用，向下用 item.h 的页面类，
 *   用 hal 画屏、读按键。
 *
 * 功能：
 *   开页和返回
 *   处理按键
 *   弹窗提示
 */

#include "launcher.h"
#include "esp_log.h"   /* 功能：出错打串口 */

namespace astra {

static const char *TAG_L = "astra_launcher";

/* 功能：弹个提示框 */
void Launcher::popInfo(std::string _info, uint16_t _time) {
  /* 功能：按毫秒算弹窗时长 */
  const unsigned long beginMs = HAL::millis();
  ESP_LOGI(TAG_L, "popInfo 开始：%s（%u ms）", _info.c_str(), (unsigned)_time);

  float wPop = HAL::getFontWidth(_info) + 2 * getUIConfig().popMargin;  /* 功能：弹窗多宽 */
  float hPop = HAL::getFontHeight() + 2 * getUIConfig().popMargin;  /* 功能：弹窗多高 */

  float yPop = 0 - hPop - 8; /* 功能：从上面滑进来 */
  float yPopTrg = 0;

  float xPop = (HAL::getSystemConfig().screenWeight - wPop) / 2;  /* 功能：左右居中 */

  while (true) {
    time++;

    if (HAL::millis() - beginMs < _time) yPopTrg = (HAL::getSystemConfig().screenHeight - hPop) / 3;  /* 功能：停在中间偏上 */
    else yPopTrg = 0 - hPop - 8;  /* 功能：滑出去 */

    HAL::canvasClear();
    /* 功能：把页面画一帧 */
    currentPage->render(camera->getPosition());
    selector->render(camera->getPosition());
    camera->update(currentPage, selector);

    HAL::setDrawType(0);
    HAL::drawRBox(xPop - 4, yPop - 4, wPop + 8, hPop + 8, getUIConfig().popRadius + 2);
    HAL::setDrawType(1);  /* 功能：反色显示 */
    HAL::drawRFrame(xPop - 1, yPop - 1, wPop + 2, hPop + 2, getUIConfig().popRadius); /* 功能：画个圆角框 */
    /* 功能：用能显中文的画法 */
    HAL::drawChinese(xPop + getUIConfig().popMargin, yPop + getUIConfig().popMargin + HAL::getFontHeight(), _info);  /* 功能：写提示文字 */

    HAL::canvasUpdate();

    animation(&yPop, yPopTrg, getUIConfig().popSpeed);  /* 功能：慢慢移动 */

    if (HAL::millis() - beginMs >= _time && yPop == 0 - hPop - 8) break;  /* 功能：时间到就退出 */
    if (HAL::getAnyKey()) break;  /* 功能：按键就关掉 */
  }
  ESP_LOGI(TAG_L, "popInfo 结束：%s", _info.c_str());
}

/* 功能：把首屏挂上 */
void Launcher::init(Menu *_rootPage) {
  currentPage = _rootPage;

  camera = new Camera(0, 0);
  _rootPage->init(camera->getPosition());

  selector = new Selector();
  selector->inject(_rootPage);
  selector->go(_rootPage->selectIndex);

}

/* 功能：打开选中的页 */
bool Launcher::open() {
  /* 功能：开关流程先这样 */

  /* 功能：没有下页就报错 */
  if (currentPage->getNext() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  /* 功能：空页面不给打开 */
  if (currentPage->getNext()->getItemNum() == 0 && !currentPage->getNext()->openableWhenEmpty) {
    popInfo("empty page!", 600);
    return false;
  }

  currentPage->onExit();  /* 功能：通知老页面 */
  currentPage->deInit();  /* 功能：先收尾再挪指针 */

  currentPage = currentPage->getNext();
  camera->reset();  /* 功能：镜头回原位 */
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  currentPage->onEnter();  /* 功能：通知新页面 */

  ESP_LOGI(TAG_L, "open → %s", currentPage->title.c_str());
  return true;
}

/* 功能：关掉当前页 */
bool Launcher::close() {
  if (currentPage->getPreview() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  /* 功能：空页也允许返回 */

  currentPage->onExit();  /* 功能：通知老页面 */
  currentPage->deInit();  /* 功能：先收尾再挪指针 */

  currentPage = currentPage->getPreview();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  currentPage->onEnter();  /* 功能：通知新页面 */

  ESP_LOGI(TAG_L, "close ← %s", currentPage->title.c_str());
  return true;
}

/* 功能：页面变了就重刷 */
void Launcher::refreshPage(Menu* _page) {
  /* 功能：只认当前页 */
  if (_page == nullptr || _page != currentPage) return;

  if (_page->getItemNum() > 0) {
    if (_page->selectIndex >= _page->getItemNum()) _page->selectIndex = 0;
    selector->inject(_page);
  } else {
    selector->inject(_page);  /* 功能：空页只接管指针 */
  }
  camera->reset();  /* 功能：视角回到顶上 */
}

/* 功能：直接跳到指定页 */
bool Launcher::openTarget(Menu* _page) {
  /* 功能：目标页和当前页 */
  if (_page == nullptr || _page == currentPage) return false;

  currentPage->onExit();  /* 功能：通知老页面 */
  currentPage->deInit();  /* 功能：先收尾再挪指针 */

  currentPage = _page;
  camera->reset();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  currentPage->onEnter();  /* 功能：通知新页面 */

  return true;
}

/* 功能：每帧重画并收键 */
void Launcher::update() {
  HAL::canvasClear();

  currentPage->render(camera->getPosition());
  selector->render(camera->getPosition());
  camera->update(currentPage, selector);

  /* 功能：按键干活在这 */
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
      /* 功能：OK 交给页面先处理 */
      if (!currentPage->onOkKey() && itemNum > 0) open();
    } else if (i == key::KEY_LEFT) {
      /* 功能：LEFT 先问页面 */
      if (!currentPage->onLeftKey()) close();
    } else if (i == key::KEY_RIGHT) {
      /* 功能：右键留给页面用 */
      currentPage->onRightKey();
    }
  }
  HAL::clearKeyActions();

  HAL::canvasUpdate();

  time++;
}
}