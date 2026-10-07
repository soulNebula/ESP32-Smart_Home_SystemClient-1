#include "launcher.h"
// 出错打串口
#include "esp_log.h"

namespace astra {

static const char *TAG_L = "astra_launcher";

// 弹个提示框
void Launcher::popInfo(std::string _info, uint16_t _time) {
  // 按毫秒算弹窗时长
  const unsigned long beginMs = HAL::millis();
  ESP_LOGI(TAG_L, "popInfo 开始：%s（%u ms）", _info.c_str(), (unsigned)_time);

  // 弹窗多宽
  float wPop = HAL::getFontWidth(_info) + 2 * getUIConfig().popMargin;
  // 弹窗多高
  float hPop = HAL::getFontHeight() + 2 * getUIConfig().popMargin;

  // 从上面滑进来
  float yPop = 0 - hPop - 8;
  float yPopTrg = 0;

  // 左右居中
  float xPop = (HAL::getSystemConfig().screenWeight - wPop) / 2;

  while (true) {
    time++;

    // 停在中间偏上
    if (HAL::millis() - beginMs < _time) yPopTrg = (HAL::getSystemConfig().screenHeight - hPop) / 3;
    // 滑出去
    else yPopTrg = 0 - hPop - 8;

    HAL::canvasClear();
    // 把页面画一帧
    currentPage->render(camera->getPosition());
    selector->render(camera->getPosition());
    camera->update(currentPage, selector);

    HAL::setDrawType(0);
    HAL::drawRBox(xPop - 4, yPop - 4, wPop + 8, hPop + 8, getUIConfig().popRadius + 2);
    // 反色显示
    HAL::setDrawType(1);
    // 画个圆角框
    HAL::drawRFrame(xPop - 1, yPop - 1, wPop + 2, hPop + 2, getUIConfig().popRadius);
    // 用能显中文的画法
    // 写提示文字
    HAL::drawChinese(xPop + getUIConfig().popMargin, yPop + getUIConfig().popMargin + HAL::getFontHeight(), _info);

    HAL::canvasUpdate();

    // 慢慢移动
    animation(&yPop, yPopTrg, getUIConfig().popSpeed);

    // 时间到就退出
    if (HAL::millis() - beginMs >= _time && yPop == 0 - hPop - 8) break;
    // 按键就关掉
    if (HAL::getAnyKey()) break;
  }
  ESP_LOGI(TAG_L, "popInfo 结束：%s", _info.c_str());
}

// 把首屏挂上
void Launcher::init(Menu *_rootPage) {
  currentPage = _rootPage;

  camera = new Camera(0, 0);
  _rootPage->init(camera->getPosition());

  selector = new Selector();
  selector->inject(_rootPage);
  selector->go(_rootPage->selectIndex);

}

// 打开选中的页
bool Launcher::open() {
  // 开关流程先这样

  // 没有下页就报错
  if (currentPage->getNext() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  // 空页面不给打开
  if (currentPage->getNext()->getItemNum() == 0 && !currentPage->getNext()->openableWhenEmpty) {
    popInfo("empty page!", 600);
    return false;
  }

  // 通知老页面
  currentPage->onExit();
  // 先收尾再挪指针
  currentPage->deInit();

  currentPage = currentPage->getNext();
  // 镜头回原位
  camera->reset();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  // 通知新页面
  currentPage->onEnter();

  ESP_LOGI(TAG_L, "open → %s", currentPage->title.c_str());
  return true;
}

// 关掉当前页
bool Launcher::close() {
  if (currentPage->getPreview() == nullptr) { popInfo("unreferenced page!", 600); return false; }
  // 空页也允许返回

  // 通知老页面
  currentPage->onExit();
  // 先收尾再挪指针
  currentPage->deInit();

  currentPage = currentPage->getPreview();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  // 通知新页面
  currentPage->onEnter();

  ESP_LOGI(TAG_L, "close ← %s", currentPage->title.c_str());
  return true;
}

// 页面变了就重刷
void Launcher::refreshPage(Menu* _page) {
  // 只认当前页
  if (_page == nullptr || _page != currentPage) return;

  if (_page->getItemNum() > 0) {
    if (_page->selectIndex >= _page->getItemNum()) _page->selectIndex = 0;
    selector->inject(_page);
  } else {
    // 空页只接管指针
    selector->inject(_page);
  }
  // 视角回到顶上
  camera->reset();
}

// 直接跳到指定页
bool Launcher::openTarget(Menu* _page) {
  // 目标页和当前页
  if (_page == nullptr || _page == currentPage) return false;

  // 通知老页面
  currentPage->onExit();
  // 先收尾再挪指针
  currentPage->deInit();

  currentPage = _page;
  camera->reset();
  currentPage->init(camera->getPosition());

  selector->inject(currentPage);
  // 通知新页面
  currentPage->onEnter();

  return true;
}

// 每帧重画并收键
void Launcher::update() {
  HAL::canvasClear();

  currentPage->render(camera->getPosition());
  selector->render(camera->getPosition());
  camera->update(currentPage, selector);

  // 按键干活在这
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
      // OK 交给页面先处理
      if (!currentPage->onOkKey() && itemNum > 0) open();
    } else if (i == key::KEY_LEFT) {
      // LEFT 先问页面
      if (!currentPage->onLeftKey()) close();
    } else if (i == key::KEY_RIGHT) {
      // 右键留给页面用
      currentPage->onRightKey();
    }
  }
  HAL::clearKeyActions();

  HAL::canvasUpdate();

  time++;
}
}