# ESP32-S3 智能家居（语音 + 手机 App + 自动联动）

> **一句话**：一块 ESP32-S3 把 4 路灯带、3 路舵机（窗帘/窗户/门）、风扇、温湿度/光照/雨滴传感器、OLED 和语音模块串起来，
> 支持 **语音 / 按键 / 串口 / 手机 App(MQTT)** 四种控制入口，并按阈值自动联动。
> 配件没到齐也能先烧录运行 —— 缺哪个外设只影响那一个功能，其余照常。

开发板：**ESP32-S3-DevKitC-1**（模组 **ESP32-S3-WROOM-1-N16R8**，16MB Flash + 8MB Octal PSRAM）
框架：**ESP-IDF v5.4.x**（本机装在 `E:\Espressif\frameworks\esp-idf-v5.4.4`）

---

## ⚠️ 构建前必读：工程路径**不能含中文**（本工程实测踩过）

本工程当前位于 `C:\Users\Administrator\Desktop\esp32智能家居_客户`，
**这个中文路径会让 ESP-IDF 在 Windows 上构建失败**。已实测确认它会在三个阶段连环出错：

| 阶段 | 报错信息 | 原因 |
|------|---------|------|
| kconfig | `FileNotFoundError: .../build/kconfigs.in` | `$IDF_PATH/tools/kconfig_new/prepare_kconfig_files.py` 用 `argparse.FileType('r')` 打开 `build/config.env`，走的是系统 ANSI 代码页（本机 936/GBK），而 CMake 写的是 UTF-8 → 路径被解码成乱码 |
| 编译 | `ccache: terminate ... filesystem error: Cannot convert character sequence: Illegal byte sequence` | ccache 用 `std::filesystem` 按本地代码页转换路径，遇到非 ASCII 直接抛异常 |
| 链接 | `xtensa-esp32s3-elf-objdump.exe: .../libxtensa.a: No such file or directory` | `$IDF_PATH/tools/ldgen/ldgen.py` 调用 `objdump.exe` 时路径被 ANSI 转换弄坏（该库其实存在，66938 字节） |

### ✅ 推荐做法：把工程移到纯 ASCII 路径

```powershell
Move-Item "C:\Users\Administrator\Desktop\esp32智能家居_客户" C:\esp32_smart_home
Set-Location C:\esp32_smart_home
idf.py set-target esp32s3
idf.py build
```

**已在 `C:\esp32_smart_home` 实测通过**：

```text
Project build complete.
esp32_smart_home.bin  binary size 0xf71a0 bytes (988 KB)
Smallest app partition is 0x400000 bytes. 0x308e60 bytes (76%) free.
Bootloader binary size 0x5760 bytes.
```

### 🩹 如果必须保留中文路径（本工程当前就是这种情况）

用 [`tools/run_idf.ps1`](tools/run_idf.ps1) —— 它会把源码**自动镜像到纯 ASCII 目录**再构建：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task build                         # 编译
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task erase-flash,flash -Port COM31  # 擦除+烧录
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task monitor -Port COM31           # 看串口
```

> 📌 **ESP-IDF 环境现在从哪来**：本机原来的 `E:\Espressif\frameworks\esp-idf-v5.4.4` 已被删除。
> 现在由 [`tools/idf_pio.ps1`](tools/idf_pio.ps1) 复用 **PlatformIO 自带的 ESP-IDF 5.4.0 + gcc 14.2.0**，
> 在 **`<项目>\.idf-sandbox`** 里建一个**只服务本工程**的 Python 环境（不写任何全局/用户环境变量），
> 卸载只需删掉这个目录。详见 [docs/11](docs/11-五位AD键盘修复与调试.md) 第 6 节。

> ⚠️ **用 `powershell`（Windows PowerShell 5.1），不要用 `pwsh`。**
> `pwsh` 是 PowerShell 7，要单独安装；本机**没装**，敲 `pwsh` 会直接报
> "不是可识别的命令"。`powershell` 是系统自带，一定可用。
>
> ⚠️ **本机这颗板子烧录前必须先全片擦除。** 直接 `flash` 会在写
> `ota_data_initial.bin`（内容全是 `0xFF`）时报 `MD5 of file does not match data in flash!`
> —— 因为 NOR Flash 写 `0xFF` 清不掉已有数据。所以统一用上面那条
> `-Task erase-flash,flash`（已实测：先擦后烧会 4 段全部 `Hash of data verified`）。
> 注意：擦除会清掉 NVS，**App 里设过的联动阈值会回到默认值**，需重设一次。
>
> 💡 **只是想看日志/敲命令**，不必走构建脚本，直接用串口监视器：
>
> ```powershell
> powershell -ExecutionPolicy Bypass -File tools\monitor.ps1 -Port COM31
> ```

内部流程：`robocopy 中文目录 → C:\esp32_smart_home → idf.py build`，构建完再把 `sdkconfig` 回写。
**你始终在自己目录里改代码，完全不用管镜像目录。** 实测输出：

```text
[WARN] Project path contains NON-ASCII characters.
       Mirroring sources to the ASCII path below and building THERE:
       C:\esp32_smart_home
[OK]   sources synced (robocopy exit 3)
Project build complete.
esp32_smart_home.bin binary size 0xf72a0 bytes (989 KB)
[OK]   sdkconfig copied back to the source project
```

> ⚠️ 试过用 `subst X: "中文路径"` 挂一个 ASCII 盘符来绕过，**不管用** ——
> CMake 会把盘符解析回真实路径，中文依旧泄漏到 `build/config.env` 里，构建照样失败。
> 所以「镜像到 ASCII 目录」是目前唯一可靠的做法。

---

## 功能概览

| # | 功能 | 控制入口 | 代码位置 |
|---|------|----------|----------|
| 1 | 光照低于/高于阈值 → 客厅灯开、窗帘合（迟滞双阈值） | 自动 | `components/App/automation.c` 规则1 |
| 2 | 温湿度采集 → OLED 显示；高温开风扇、低温关 | 自动 / 语音 | `SENSOR/sensor.c`、`OLED/oled.c`、规则2 |
| 3 | 雨滴传感器 → 关窗（雨停可选自动重开） | 自动 | `automation.c` 规则3 |
| 4 | 客厅/厨房/卧室/浴室 4 路灯带开关 + 亮度 + 颜色 | 语音 / 按键 / 串口 / MQTT / BLE | `LED/led.c`、`device_model.c` |
| 5 | 舵机开合门（另有窗户、窗帘共 3 路） | 语音 / 串口 / MQTT / BLE | `SERVO/servo.c` |
| 6 | 语音控制全部设备 + 播报当前温湿度 | 语音 | `VOICE/voice.c` + `main.c` 的 `voice_on_cmd()` |
| 6a | **语音方案 A（默认）**：外部 ASRPRO 模块，UART1 串口命令词 | 语音 | `VOICE/voice.c`（`CONFIG_APP_VOICE_SOURCE_ESP_SR = n`） |
| 6b | **语音方案 B（可选）**：板载 **INMP441 I²S 麦克风 + 乐鑫 ESP-SR 离线识别**（唤醒词「你好小智」+ 25 条中文命令词，不联网不上云） | 语音 | `I2S_MIC/i2s_mic.c` + `VOICE/voice_esp_sr.c`（`CONFIG_APP_VOICE_SOURCE_ESP_SR = y`），见 [`docs/13-语音模块-ESP-SR.md`](docs/13-语音模块-ESP-SR.md) |
| 7 | 手机 App / 上位机控制全部设备、查看状态 | **MQTT（WiFi，可远程）/ BLE（蓝牙，本地直连）** | `App/mqtt_app.c`、`App/ble_app.c`，两条链路共用 `App/app_link.c` 的广播 |

> **6a / 6b 是二选一的开关**，识别结果都会被翻译成同一个 `voice_cmd_t` 走同一条下游链路，
> 所以 MQTT / OLED / 自动联动 / `main.c::voice_on_cmd()` **一行都不用改**。

## 硬件清单

| 器件 | 规格建议 | 数量 | 已到货☐ |
|------|----------|------|---------|
| 开发板 | ESP32-S3-DevKitC-1（N16R8） | 1 | ☐ |
| WS2812B 灯带 | 5V，每路 30 颗（数量见 `BSP_WS2812_LED_NUM_*`） | 4 条 | ☐ |
| 舵机 | SG90（轻载）或 MG996R（大扭矩） | 3 | ☐ |
| 温湿度模块 | SHT30（0x44）或 AHT20（0x38），I2C | 1 | ☐ |
| 光照模块 | BH1750（0x23）可选；或光敏电阻 GL5528 + 10kΩ | 1 | ☐ |
| 雨滴模块 | AO 模拟量输出（DO 可选接 GPIO12） | 1 | ☐ |
| OLED | 0.96" SSD1306 128x64，I2C，地址 0x3C | 1 | ☐ |
| MOS 管模块 | **AO3400 / IRLZ44N**（3.3V 逻辑电平可完全导通；**别用 IRF520 模块**，Vgs(th) 偏高会半导通发热） | 1 | ☐ |
| 风扇 | 4 线 PWM 风扇或普通 DC 风扇 | 1 | ☐ |
| 独立 5V 电源 | **5V/3A 以上**（舵机必须独立供电） | 1 | ☐ |
| 语音模块 | SU-03T / ASRPRO / LD3320（串口命令词） | 1 | ☐ |
| 数字麦克风（可选） | **INMP441**（I²S 接口，★**不是 I²C**），配 ESP-SR 做离线唤醒词+命令词 | 1 | ☐ |
| 杜邦线 / 端子 / 面包板 | 若干 | — | ☐ |

> **实际买到的器件、规格核对、以及"哪些买错了/怎么适配"→ 看 [`docs/07-元器件采购订单.md`](docs/07-元器件采购订单.md)。**
> 引脚与接线供电见 [`docs/03-引脚分配表.md`](docs/03-引脚分配表.md)、[`docs/04-接线与供电指南.md`](docs/04-接线与供电指南.md)。

## 目录结构

```text
esp32智能家居_客户/
├── CMakeLists.txt                 顶层构建脚本（说明组件发现规则）
├── partitions-16MiB.csv           16MB Flash 分区表：双 OTA(各4MB) + model(4MB) + 3.875MB spiffs
├── sdkconfig.defaults             默认配置：Octal PSRAM / 240MHz / MQTT 等
├── README.md                      本文件
├── components/
│   ├── BSP/                       ← 组件① 板级支持包：所有硬件驱动与引脚真源
│   │   ├── board_config.h         ★ 全工程唯一引脚真源，改引脚只改这里
│   │   ├── board.c/.h             board_init() 逐个 try 初始化，失败只告警
│   │   ├── ADKEY/  KEY/  LED/  I2C/  ADC/  WS2812/  SERVO/  FAN/  SENSOR/  OLED/
│   │   ├── I2S_MIC/               ← INMP441 I²S 数字麦克风驱动（★ I²S 不是 I²C）
│   │   └── VOICE/                 ← 语音抽象层 + 两套识别来源
│   │       ├── voice.c/.h         语音抽象层：voice_cmd_t + 回调 + ASRPRO UART 解析
│   │       ├── voice_internal.h   voice_dispatch()：两套识别来源的唯一汇合点
│   │       └── voice_esp_sr.c/.h  ESP-SR 离线识别（★ 命令词表在这个文件里）
│   ├── App/                       ← 组件② 业务层：设备模型 / 联动 / WiFi / MQTT
│   │   ├── device_model.c/.h      全部设备的统一状态机（唯一控制汇聚点）
│   │   ├── automation.c/.h        光照/温度/雨滴三条联动规则 + 阈值 NVS 持久化
│   │   ├── wifi_sta.c/.h          WiFi STA 自动重连、MAC 后缀唯一 ID
│   │   ├── mqtt_app.c/.h          esp-mqtt 客户端、订阅下发、自动上报
│   │   └── mqtt_protocol.c/.h     ★ topic 规划与 JSON 格式（文档第 5 篇严格照此）
│   ├── astra_ui/                  ← 组件③ OLED 菜单框架（u8g2 画布 + 五键导航）
│   └── u8g2/                      ← 组件④ 图形库（含 wqy12 中文字体）
│                                  （managed_components/ 由组件管理器生成：
│                                    esp-sr / esp-dsp / dl_fft —— 仅 ESP-SR 语音方案需要）
├── main/
│   ├── main.c                     应用入口、任务拓扑、串口调试台（mic / voice-test）
│   ├── astra_glue.cpp             页面数据刷新 + 五键 → UI 桥接
│   ├── Kconfig.projbuild          全部 CONFIG_APP_* 配置项（含语音来源开关）
│   └── idf_component.yml          依赖 IDF>=5.4.0，以及 esp-sr 2.3.1（说明见文件内）
├── docs/                          文档（01~14，索引见文末）
└── tools/
    ├── build.ps1                  ★ 编译/烧录入口（镜像到纯 ASCII 路径再调 idf.py）
    ├── run_idf.ps1                旧入口（用 PlatformIO 自带 IDF 沙箱），保留备用
    ├── idf_pio.ps1                同上，实际实现
    ├── monitor.ps1                ★ 串口监视器：看日志 + 直接下发命令（日志自动落盘）
    ├── monitor.cmd                同上，双击即可运行
    ├── verify_adkey.ps1           ★ 五位键盘实机验证（读串口自动判定，支持 -Replay 回放）
    ├── run_adkey_test.ps1         ★ 判键逻辑 PC 端单测（MinGW gcc，不用板子）
    ├── adkey_test.c               同上，测试源码
    ├── run_ui_preview.ps1         ★ 128x64 排版预览 + 行重叠检测（不用板子）
    ├── ui_layout_preview.cpp      同上，预览源码
    ├── check_comments.ps1         ★ C/C++ 注释结构自检（抓"注释把代码搞坏"那类难查的错）
    └── ble_probe.py               PC 端 BLE 探针：在电脑上验证板子的 GATT 服务（不需要手机）
```

## 快速开始

### 1. 准备 ESP-IDF 环境（本机专用沙箱，不用自己装 IDF）

> 📌 本机原来的 `E:\Espressif\frameworks\esp-idf-v5.4.4` **已被删除**。现在不需要再装一份：
> [`tools/run_idf.ps1`](tools/run_idf.ps1) 复用 **PlatformIO 自带的 ESP-IDF 5.4.0 + gcc 14.2.0**，
> 并在 `<项目>\.idf-sandbox` 建一个**只服务本工程**的 Python 环境（不写任何全局/用户环境变量；
> 卸载只需删掉这个目录）。首次运行会自动装依赖（走清华镜像，约 1~2 分钟），之后直接可用。

```powershell
# 编译（首次会自动建沙箱）
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task build
```

详见 [docs/11](docs/11-五位AD键盘修复与调试.md) 第 6 节（含踩过的 10 个环境坑）。

### 2. 目标芯片

`sdkconfig` 里已经是 **esp32s3**，不用再 `set-target`。要换目标才需要：

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task set-target
```

### 3. menuconfig 里必须改的 3 项

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_idf.ps1 -Task menuconfig
```

进入 **`智能家居 —— 项目配置`**：

| 位置 | 配置项 | 改成 |
|------|--------|------|
| `WiFi 设置` | `WiFi 名称 (SSID)` | 你家路由器的 **2.4GHz** SSID（ESP32-S3 不支持 5GHz） |
| `WiFi 设置` | `WiFi 密码` | 对应密码 |
| `MQTT 设置` | `Broker 地址` | 公共测试：`mqtt://broker.emqx.io:1883`；自建：`mqtt://192.168.1.100:1883` |

其余项可保持默认（传感器采样周期 1000ms、OLED 启用、自动联动默认开启、串口调试台启用；`传感器上报周期` 默认 2000ms，决定 `<base>/sensor` 的推送频率）。

### 4. 编译、烧录、看日志

> **本机有两个构建入口，用 `tools\build.ps1`**（它对应 `E:\Espressif` 下的
> **ESP-IDF v5.4.4**，也是本工程 `dependencies.lock` 里记的版本）。
> `tools\run_idf.ps1`（走 PlatformIO 自带的那套 IDF）保留作为备用。

```powershell
# 编译
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Task build

# 烧录（本机必须先全片擦除，见上一节；COM31 换成你的实际串口）
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Task erase-flash,flash -ExtraArgs '-p','COM31'

# 看日志 / 敲命令（也可双击 tools\monitor.cmd，它会自动列串口让你选）
powershell -ExecutionPolicy Bypass -File tools\monitor.ps1 -Port COM31
```

> ⚠️ `-ExtraArgs '-p','COM31'` 里的逗号是**必须的写法**：PowerShell 5.1 把数组
> 展开给原生程序时会用逗号拼接，`tools\build.ps1` 里专门加了一步按逗号再拆一次，
> 所以必须写成这个逗号形式（写成 `-ExtraArgs '-p' 'COM31'` 反而会报
> `Missing an argument for parameter 'ExtraArgs'`）。
> 用 `run_idf.ps1` 那条路则是 `-Port COM31`，见
> [`docs/11-五位AD键盘修复与调试.md`](docs/11-五位AD键盘修复与调试.md) 里的说明。

**烧录前额外跑一次注释结构自检**（可选，但踩过坑就值得）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\check_comments.ps1
```

它专门抓"注释把代码结构搞坏"这一类问题（块注释内出现注释起始符、
块注释里某行以反斜杠结尾、块注释被提前关闭），这类问题的编译报错位置
往往会指到别处，非常难查（本工程集成 ESP-SR 时连踩三次）。

退出 monitor：`Ctrl + C`（脚本里也支持 `Ctrl + P` 回端口菜单）。

开机日志里会打印引脚表、I2C 扫描结果和你的 **MQTT topic**，例如：

```text
I (1234) mqtt_app: topics: state=smarthome/a1b2c3/state  cmd=smarthome/a1b2c3/cmd/#  ...
```

### 5. 配件还没到？先用串口调试台

在 monitor 里直接输入命令（`CONFIG_APP_SERIAL_DEBUG_ENABLE` 默认开启）：

```text
help                     查看全部命令
status                   查看设备 + 传感器 + 网络 + 联动阈值
on led_living            开客厅灯
off all                  全部关闭
set fan 60               风扇 60% 转速
color led_kitchen 255 0 0
say led_living_on        ★ 模拟"说了一句话"，走完整语音链路
help-voice               列出全部语音指令名（25 条，不含 none）
```

## 常见问题速查（FAQ）

1. **WiFi 连不上**：ESP32-S3 只支持 **2.4GHz**，5GHz SSID 一定失败；再确认 SSID/密码没有多余空格、路由器没开 MAC 过滤。日志里 `wifi_sta: disconnected (reason=...)` 的 reason 码可直接查表。
2. **I2C 扫不到设备**：确认地址（OLED `0x3C`、SHT30 `0x44`、AHT20 `0x38`、BH1750 `0x23`）、SDA=GPIO8 / SCL=GPIO9 没接反、模块供电 3.3V（不是 5V），并保证上拉电阻存在（多数成品模块已自带 4.7kΩ；没有就外挂 4.7kΩ 到 3V3）。开机日志会自动扫一遍总线。
3. **舵机抖动或 ESP32 复位**：几乎都是**供电不足**。SG90 堵转约 700mA，MG996R 可达 2.5A，开发板 LDO 带不动。必须用**独立 5V 电源**给舵机，并且**独立电源的 GND 与开发板 GND 接在一起（共地）**，否则 PWM 信号无效。
4. **灯带颜色错乱**：WS2812 是 **GRB** 顺序，本工程代码已按 GRB 处理（见 `ws2812.c`）；若仍错乱，多半是买到了 RGB 顺序的兼容品，或数据线太长/电平不匹配（5V 灯带用 3.3V 数据建议加 74HCT245 电平转换）。另外每颗灯珠满亮约 60mA，30 颗全白 ≈ 1.8A，**必须外接 5V 电源，不能从开发板 5V 引脚取电**。
5. **OLED 不亮**：驱动先探测 **0x3C**，找不到会自动回退到 **0x3D**（两种模块都能用，无需改代码）。若两个地址都扫不到，查 I2C 接线与供电。OLED 没插不会报错，`oled_is_ready()` 返回 false，所有绘制自动变成空操作。
6. **MQTT 连不上或收不到消息**：topic 里的**唯一 ID 默认取 MAC 后 3 字节**（开机日志会打印），自己拼 topic 拼错了就收不到。正确写法是 `smarthome/<ID>/cmd`，订阅用 `smarthome/<ID>/state` 或通配 `smarthome/+/state`。另外公共 broker 上不要用重复的 `client_id`（本工程为 `esp32sh-<ID>`）。
7. **OLED 中文显示成方块**：现在屏幕文字由 **astra UI + u8g2** 渲染，用的是 u8g2 自带的中文字体
   `u8g2_font_wqy12_t_gb2312`（**字符集是 GB2312**，行高 13px）。显示成方块只有两种可能：
   ① **那个字不在 GB2312 里**（生僻字/emoji/特殊符号）→ 换成常见同义字最省事；
   ② 想换更大字号的字体 → 改 [`components/astra_ui/astra/config/config.h`](components/astra_ui/astra/config/config.h)
   的 `mainFont`（例如 `u8g2_font_wqy16_t_gb2312`），**但行高会变、排版要重排** ——
   改完先跑 `tools\run_ui_preview.ps1` 看有没有行重叠，再烧板子。
   > 早期那套"自绘 6x8/16x16 字库 + `tools/gen_font.ps1` 生成器"已经删除（换成 u8g2 后全工程零调用）。
8. **编译报 sdkconfig 冲突 / 改了 `sdkconfig.defaults` 没生效**：`sdkconfig` 一旦生成就不再被 `.defaults` 覆盖。删掉 `sdkconfig` 和 `sdkconfig.old` 后重新 `idf.py set-target esp32s3`（注意 `sdkconfig` 已在 `.gitignore` 里）。
9. **板载状态灯不亮**：DevKitC-1 **v1.0 的 RGB 灯在 GPIO38，v1.1 在 GPIO48**，本工程用的是 `BSP_STATUS_LED_GPIO = GPIO48`（对应 v1.1）。v1.0 板子需改这个宏。
10. **串口调试台没反应**：确认 menuconfig 里 `应用行为 → 启用串口调试台` 是开的，并且串口工具是 **115200 8N1**、没有开"本地回显/快捷键抢占"；monitor 下直接敲字符即可。
11. **蓝牙连不上 / 找不到 "蓝牙串口" 服务**：**ESP32-S3 硬件上就没有蓝牙经典（BR/EDR），只有 BLE**，所以 **SPP 串口透传方案根本不可用** —— 网上大量"ESP32 蓝牙串口助手"教程在 S3 上注定失败。本工程用的是 **BLE + GATT 自定义服务**（见 [`docs/09-BLE协议.md`](docs/09-BLE协议.md)）。另外两个高频原因：① 手机端**必须先协商 MTU（≥256）**，否则约 250 字节的 state JSON 发不下；② 板子的**天线没插**，信号太弱扫不到。
12. **BLE 控制没反应，但 MQTT 正常**：检查 `sdkconfig` 里 `CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`、以及 **`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`**（WiFi/BLE 共用同一个 2.4GHz 射频，不开共存会互相干扰）。注意共存符号名是 `ESP_COEX_SW_COEXIST_ENABLE`，写成 `SW_COEXIST_ENABLE` 是无效的。
13. **电脑想验蓝牙但没有手机 App**：用 [`tools/ble_probe.py`](tools/ble_probe.py)（`pip install bleak`），它能在 PC 上扫描、连接、协商 MTU、订阅通知并解析 JSON：`python tools\ble_probe.py test`。
14. **串口一直被 BLE 日志刷屏、命令敲不进去** ★：BLE 连上后 NimBLE 会持续打 `GATT/GAP procedure initiated` 之类的 INFO 日志。两个办法：
    - **固件侧**：串口调试台里敲 **`log quiet`** → 把 BLE / 无线协议栈的 tag 压到 WARN，**其它日志照常**；要看 BLE 细节敲 `log all`，看完再 `log quiet`。另有 `log w`（全局只留 WARN/ERROR）、`log n`（全静音）、`log d NimBLE`（单 tag）。
      **开机默认已经压制了 BLE 三个 tag**（`main.c` 里的 `s_ble_tags`），所以不敲命令通常也是安静的。
    - **监视器侧**：按 **`Ctrl + L`** 只显示调试台输出、隐藏 `ESP_LOG` 行（**日志文件里仍完整保留**，事后可翻查）。
15. **`idf.py flash` 报 `MD5 of file does not match data in flash`，板子进启动循环** ★：这是**本机实测的必然现象** —— 普通烧录校验必失败，换波特率（460800 / 115200）报的 Flash MD5 完全一样，不是通信抖动。
    **必须先全片擦除再烧**：
    ```powershell
    python -m esptool --chip esp32s3 -p COM31 -b 460800 erase_flash
    # 擦完再 flash，就会 4 段全部 Hash of data verified
    ```
    擦除会清掉 NVS（自动化阈值回默认值、WiFi 首次重新校准），不影响功能。
16. **电脑上串口太多，不知道哪个是开发板 / 想换设备看** ★：用 [`tools/monitor.ps1`](tools/monitor.ps1)（或双击 `tools\monitor.cmd`）：
    - 启动时**自动扫描并列出所有串口 + 设备名**，输序号即可选；**波特率也能选**（默认 115200）
    - 选择会被记住（存在 `%LOCALAPPDATA%\esp32-smarthome\monitor-last.json`），下次直接回车
    - **运行中按 `Ctrl + P`** 重新扫描并切换端口/波特率，**不用退出程序**
    - **`Ctrl + L`** 只看调试台输出（隐藏 `ESP_LOG` 刷屏行，日志文件仍完整保留）
    - `-List` 只扫描列端口不进监视；`-Port COM31 -Baud 115200` 直接指定、跳过交互

    > 本机实测扫到 **5 个**串口：`COM3/4/8/9` 是**蓝牙虚拟串口**，**`COM31` 才是开发板**

17. **光照读数反了（关灯反而显示 100%）/ 灯被自动联动关掉** ★：光敏模块的 AO 极性有两种，
    本工程默认按"越亮电压越高"换算；**实测本机的模块是反极性**（越暗电压越高，关灯时
    AO 顶到 3128mV 满量程 → 被算成 100% ≈ 500lux），于是自动联动误以为"屋里有 500lux"，
    把刚打开的客厅灯又关掉、把窗帘拉开。

    - **怎么自查**：串口敲 `status`，看传感器那行 `L:xx%(NNNNmV)`：
      遮住光敏/关灯应接近 **0%（mV 低）**，强光照射应接近 **100%（mV 高）**。
    - **怎么改**：[`components/BSP/board_config.h`](components/BSP/board_config.h) 里的
      **`BSP_LIGHT_ADC_INVERT`**（0=正极性 / 1=反极性，本机设为 **1**），改完重新编译烧录。
    - **阈值和屏幕数字的关系**：自动联动用的是 **lux**，OLED/status 显示的是 **百分比**，
      两者是平方关系 `lux = %² / 20`（50lux≈32%，200lux≈63%，500lux=100%）。
      App 的「自动模式与阈值」对话框里填的是 lux，按这个换算填即可。
    - **手动优先**：手动开关某个设备后 **60 秒内**自动联动不会覆盖它
      （`AUTO_MANUAL_GUARD_MS`，见 [docs/02](docs/02-系统架构.md) 的设计决策表）。

18. **雨滴检测到下雨，但窗户没动**：规则③的动作条件是「**窗户开着** 且 下雨 → 关窗」——
    上电默认窗户就是**关**的，所以"下雨 → 关窗"没有任何可见动作（日志里也不会有 `rule3` 行）。
    想验证：先在 App 里把**窗户打开**，再往雨滴感应板上滴水，就会看到
    `rule3: rain xx% > 30% & window open -> WINDOW CLOSE`，舵机转到关窗位置。
    另外默认 `auto_window_reopen = false`（雨停不自动开窗，安全考虑），需要可在阈值对话框里打开。
    > —— 这正是以前 `idf.py` 老是去试 COM9/COM8/COM4 的原因。

## 文档索引

| 文档 | 内容 |
|------|------|
| [`docs/01-需求分析.md`](docs/01-需求分析.md) | 7 条原始需求逐条拆解 + 需求→模块对照总表 + 本期范围 |
| [`docs/02-系统架构.md`](docs/02-系统架构.md) | 分层架构、数据流、任务拓扑、组件依赖、设计取舍 |
| [`docs/03-引脚分配表.md`](docs/03-引脚分配表.md) | 全部 GPIO 分配与保留引脚 |
| [`docs/04-接线与供电指南.md`](docs/04-接线与供电指南.md) | 逐器件接线图与供电方案 |
| [`docs/05-MQTT协议.md`](docs/05-MQTT协议.md) | topic 规划、JSON 全表、MQTTX 实测步骤 |
| [`docs/07-元器件采购订单.md`](docs/07-元器件采购订单.md) | **★ 实际采购核对**：买到了什么、哪些规格不对（360° 舵机、单色 LED 模块）、下单关键词与参考价 |
| [`docs/07-元器件采购订单.csv`](docs/07-元器件采购订单.csv) | 同上，Excel 版（UTF-8 带 BOM，双击即可打开） |
| [`docs/08-接线施工表.md`](docs/08-接线施工表.md) | **★ 照着插线的施工表**：电源分轨、逐器件接线表、施工顺序、上电检查清单 |
| [`docs/接线图.svg`](docs/接线图.svg) | **★ 一页接线图**：电源分配 + GPIO 分配 + 易错点（浏览器可直接打开/打印） |
| [`docs/09-BLE协议.md`](docs/09-BLE协议.md) | **★ 手机蓝牙直连协议**：为什么 ESP32-S3 用不了 SPP、GATT UUID、帧格式、MTU 陷阱、连接时序、nRF Connect 测法 |
| [`docs/10-扩展板接线对照表.md`](docs/10-扩展板接线对照表.md) | **★ 扩展板（GPIO Extension Board）孔位对照**：丝印孔位 ↔ GPIO 号、3V3/5V/DC 座怎么用、禁用孔位、打勾施工清单 |
| [`docs/11-五位AD键盘修复与调试.md`](docs/11-五位AD键盘修复与调试.md) | **★ 五位 AD 键盘（IO10）**：OK 键失效的根因（0mV 档被吃掉）、修复内容、实测分压表、标定流程、**PC 端判键逻辑单测**（`tools/run_adkey_test.ps1`），以及 **IDF 删除后如何编译/烧录**（`tools/run_idf.ps1`） |
| [`docs/12-OLED排版与PC预览工具.md`](docs/12-OLED排版与PC预览工具.md) | **★ OLED 排版（128×64）**：实测字体行高、行盒预算、主页/传感器页重排，以及 **PC 端排版预览工具**（`tools/run_ui_preview.ps1`，不烧板子就能看排版+检测行重叠） |
| [`docs/13-语音模块-ESP-SR.md`](docs/13-语音模块-ESP-SR.md) | **★ INMP441 + ESP-SR 离线语音（插上线就能测）**：六根线接哪（**强调是 I²S 不是 I²C**）、唤醒词「你好小智」、**完整 25 条中文命令词表**、用串口 `mic` 命令**不用说话就验证麦克风**、`voice-test` 打印命令词表、`model` 分区与模型体积、四条排错清单、ASRPRO↔ESP-SR 方案对比 |
| [`docs/14-商业化定价分析.md`](docs/14-商业化定价分析.md) | **★ 商业化定价分析**：硬件 BOM（原型 ¥166 / 量产 ¥145~175）、软件开发成本按模块折算（125~174 人天 ≈ ¥12.5~22.6 万）、三种商业模式定价建议（DIY 套件 ¥499~699 / 量产成品 ¥399~599 / ODM 方案费）、回本测算（约 527 套）、距可量产还差的 ¥15~35 万工程化投入 |
