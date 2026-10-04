# 12 · OLED 排版（128×64）与 PC 端预览工具

> **现象**：主页上除了温湿度大字，其它文字挤在一起看不清。
> **根因**：行基线是"凭手感"给的，没有按字体的实际行高做预算 —— 后三行只差 0~7px。
> **状态**：✅ 已修复、已烧录（2026-09-29）。**改排版可以先用本文的预览工具在电脑上看，不用烧板子。**

---

## 1. 先量清楚：这套 u8g2 里字到底多高

用本文的预览工具（`tools/run_ui_preview.ps1`）实测：

| 字体 | 用途 | 行高（`u8g2_GetMaxCharHeight`） | 中文字形墨迹高 |
|---|---|---|---|
| `u8g2_font_wqy12_t_gb2312` | 主字体（中文 + ASCII） | **13px** | 11px |
| `u8g2_font_10x20_mn` | 温湿度大字号（只有数字/ASCII） | **17px** | 13px（"25.3C 52%"） |

> ⚠️ 这套库里**最小的中文字体就是 wqy12**（只有 wqy12~wqy16），所以中文行最低也要 13px。
> 64px 高的屏 ⇒ **最多 4 行**（标题 + 大字号 + 2 行中文）。想放第 5 行必然要叠。
> ⚠️ `10x20_mn` 里**没有中文字形** —— 把带中文的串（如 `"温度 24.9C"`）整个交给它，
> "温度"会画成缺字。要中文标签就把标签用 wqy12、数值用大字号拼在同一行。

---

## 2. 主页为什么会糊

旧代码的行基线：`标题=2+字高(14) / 温湿度=38 / 光照雨滴=49 / 网络=58 / 提示=62`。
用预览工具量出来的实际墨迹范围：

| 元素 | y 范围 | 与上一行的间距 |
|---|---|---|
| 标题 | 4..14 | — |
| 温湿度（大字号） | 25..37 | 10px |
| 光照/雨滴 | 39..49 | **1px** |
| 网络 | 50..58 | **0px** |
| 提示（自动联动 + "OK 菜单"） | 52..62 | **★ 重叠 7px** |

"网络"和"光照/雨滴"贴在一起、"提示"直接压在"网络"上 —— 所以除大字号外全糊。

---

## 3. 修复：把行基线抽成配置，按行盒预算排

[`components/astra_ui/astra/config/config.h`](../components/astra_ui/astra/config/config.h) 新增 4 个常数
（**主页与传感器页共用**，也是预览工具读取的同一份数字）：

```cpp
float rowTitleY = 11;   // 标题行     行盒 0..13
float rowBigY   = 29;   // 大字号行   行盒 13..34
float row3Y     = 45;   // 第三行     行盒 34..47
float row4Y     = 58;   // 第四行     行盒 47..60
```

**主页**（[`astra_rocket.cpp`](../components/astra_ui/astra/astra_rocket.cpp) `HomePage::render`）压成 4 行：

| 行 | 内容 |
|---|---|
| 1 | `智能家居`（居中） ＋ 右侧 `自动开/关` |
| 2 | `25.3C 52%`（10x20 大字号） |
| 3 | `光照 65% 雨滴 6%` |
| 4 | `WiFi OK  MQTT OK` |

> 原来的第 5 行"`自动开 OK 菜单`"提示取消了 —— 64px 放不下 5 行；自动联动状态挪到标题行右侧。
> "OK 可以进菜单"这条提示仍在"关于"页里。

**传感器页**（`SensorPage::render`）同样重排：标题行右侧放"返回"，然后 温度(大字号, 中文标签 + 数值拼行) /
湿度 / 光照(左) + 雨滴(右)。顺带修掉了原来"提示和大字号都画在 y=63"的重叠、以及把 `"温度 24.9C"`
整串丢给英文大字号导致的缺字。

修复后的预览结果：**真正重叠 0 处**，行间距 4~6px。

---

## 4. 预览工具：改排版先在这里看，别急着烧板子

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_ui_preview.ps1
```

它做的事：
1. 把 `tools/ui_layout_preview.cpp` + 工程自己的 u8g2 源码 + `config.h` 拷到纯 ASCII 暂存目录；
2. 用 MinGW gcc/g++ 编成 PC 程序；
3. 用**和固件同一份行基线、同一批字模**把 128×64 屏幕渲染到 RAM 缓冲；
4. 输出：
   - 逐元素的**墨迹包围盒**（x/y 范围、行高、与上一行的间距）；
   - **行盒占用图**（每格 1px，一眼看出哪几行挤在一起）；
   - **ASCII 渲染图**（128×64，`#` = 点亮），可以直观看到排版；
   - **新旧对照**：同一份内容按旧坐标和现行基线各渲染一遍，报告"真正重叠 N 处"。

> 判定重叠时同时比较 x 与 y 范围 —— 同一行左右两段（如"标题 + 自动开"）不算冲突。

---

## 5. 这个工具踩过的 3 个坑（都已修）

| # | 现象 | 原因 |
|:-:|---|---|
| 1 | 链接报 `undefined reference to u8g2_font_wqy12_t_gb2312` | `u8g2.h` 只在 `unix/arm/ESP8266/ESP_PLATFORM/...` 平台宏下才定义 `U8G2_USE_LARGE_FONTS`，中文大字体（>32K）全被 `#ifdef` 掉了。ESP32 构建有 `ESP_PLATFORM`，MinGW 没有 → 必须显式 `-DU8G2_USE_LARGE_FONTS` |
| 2 | 编译通过但运行时 `0xC0000005`（访问违例） | 直接调 `u8g2_SetupBuffer()` 而没有先建立 `u8x8.display_info` → 绘制时解引用空指针。要用 `u8g2_Setup_ssd1306_i2c_128x64_noname_f()`（内部会建 display_info 并分配缓冲），再去读 `u8g2.tile_buf_ptr` |
| 3 | 运行时一句输出都没有就崩 | stdout 重定向到文件时是块缓冲，崩溃前没 flush → 看不到任何打印（不是"没跑起来"） |

---

## 6. 相关文件

| 文件 | 作用 |
|---|---|
| [`components/astra_ui/astra/config/config.h`](../components/astra_ui/astra/config/config.h) | **行基线真源**（rowTitleY/rowBigY/row3Y/row4Y）+ 主字体 |
| [`components/astra_ui/astra/astra_rocket.cpp`](../components/astra_ui/astra/astra_rocket.cpp) | `HomePage::render` / `SensorPage::render` 排版 |
| [`main/astra_glue.cpp`](../main/astra_glue.cpp) | 每帧把字符串写进页面字段（`refresh_home` / `refresh_sensor`） |
| [`tools/ui_layout_preview.cpp`](../tools/ui_layout_preview.cpp) | PC 端排版预览 + 重叠检测 |
| [`tools/run_ui_preview.ps1`](../tools/run_ui_preview.ps1) | 编译并运行上面的预览（纯 ASCII 脚本） |
| [`components/astra_ui/hal/esp32/astra_hal_esp32.cpp`](../components/astra_ui/hal/esp32/astra_hal_esp32.cpp) | u8g2 画布与绘制原语（文字坐标 = **基线**） |
