# ESP32-S3 智能家居 · Arduino 版

这是原 ESP-IDF 工程的 Arduino 移植版：**接线一根都不用改，手机 App、MQTT 主题和报文格式也都一样**。

用 Arduino IDE 打开 `esp32_smart_home/esp32_smart_home.ino` 就能编译上传，不需要装 ESP-IDF。

---

## 一、三步跑起来

### 第 1 步：装 Arduino IDE 和 ESP32 开发板包

1. 装 **Arduino IDE 2.x**（<https://www.arduino.cc/en/software>）。
2. 打开 IDE → `文件` → `首选项` → 在「附加开发板管理器网址」里填：

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. `工具` → `开发板` → `开发板管理器` → 搜 **esp32** → 装 **esp32 by Espressif Systems**。

> 2.x 和 3.x 的核心包都能编译（代码里做了兼容），装最新的就行。

### 第 2 步：按下面这张表设置开发板

`工具` 菜单里逐项选：

| 菜单项 | 选什么 | 说明 |
|---|---|---|
| 开发板 | **ESP32S3 Dev Module** | 核心板是 ESP32-S3 |
| Flash Size | **16MB (128Mb)** | N16R8 是 16MB Flash |
| PSRAM | **OPI PSRAM** | N16R8 的 8MB PSRAM 是 Octal 的，选错会起不来 |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** | 随便哪个 16M 的都行，本工程不用 OTA |
| Upload Speed | **921600**（不稳就降 115200） | 见下面"常见问题"第 3 条 |
| CPU Frequency | 240MHz | |
| USB CDC On Boot | **Disabled** | 板载是 CH343 串口芯片，走 UART0 |
| Flash Mode | QIO 80MHz | |

### 第 3 步：编译上传

1. 点左上角 **✓ 验证/编译**，应该一路通过、没有 error。
2. 插上 USB，选对串口（`工具` → `端口`，一般是 `COMx`）。
3. 点 **→ 上传**。
4. 上传完打开 **串口监视器**（右上角放大镜图标），波特率选 **115200**，就能看到开机日志。

开机日志大概长这样：

```text
###########################################################
#  ESP32-S3 智能家居 · Arduino 版
#  build ...
###########################################################
----------------- PIN MAP (source: board_config.h) -----------------
  I2C    SDA=8 SCL=9          -> OLED / AHT20
  ...
功能开关: 屏幕=开  MQTT=开  蓝牙=开  语音=开  状态灯=开
LED: init ok: 4/4 zone(s) ready
SERVO:   window  ch=1 gpio=16 -> 30 deg (closed), detached
FAN: init ok: 25000Hz / 10 bit, pwm=gpio18, stopped
...
MAIN: 串口调试台已就绪，输入 help 看命令
```

---

## 二、三个库（要么装上，要么把开关改 0）

屏幕、联网、蓝牙各要一个第三方库。**两个办法，二选一：**

**办法一（推荐）：三个库都装上** —— 功能全开，代码一个字不用改。

| 功能 | 库 | 在哪儿装 |
|---|---|---|
| OLED 屏幕 | **U8g2**（by olikraus） | `工具` → `管理库` → 搜 `U8g2` → 装 |
| WiFi + MQTT 上云 | **PubSubClient**（by Nick O'Leary） | 管理库 → 搜 `PubSubClient` → 装 |
| 手机 App 蓝牙直连 | **NimBLE-Arduino**（by h2zero） | 管理库 → 搜 `NimBLE-Arduino` → 装 |

> ⚠️ **NimBLE-Arduino 要跟开发板核心包配对**：核心包 **2.x** 装 **NimBLE-Arduino 1.4.x**，核心包 **3.x** 装 **NimBLE-Arduino 2.x**。装错版本会在编译时报一堆 NimBLE 内部的错。
> 管理库里装指定版本：点库页面右下角的版本下拉框，选 `1.4.3` 再 Install。

**办法二：一个库都不装** —— 把 `app_config.h` 里这三个开关改成 `0`：

```cpp
#define APP_OLED_ENABLE         0   // 不要屏幕
#define APP_MQTT_ENABLE         0   // 不上云
#define APP_BLE_ENABLE          0   // 不用手机蓝牙
```

这样**照样能编译、能跑**，剩下的功能都在：四路灯、窗户、门、风扇、温湿度/光照/雨滴采集、自动联动、按键和五键键盘、语音模块、串口命令台。

> 只装了一部分也行：装上哪个就把哪个开关留 `1`，没装的那个改 `0`。
> 开关开着但库没装，编译会停在一句中文提示上，告诉你装哪个库或者改哪个开关，不会只丢一句 `No such file`。

---

## 三、要改的东西都在两个头文件里

### `app_config.h` —— 功能开关 + 联网账号

```cpp
// 填自己家的 WiFi，必须是 2.4GHz（ESP32-S3 不支持 5GHz）
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"
```

- 没填就安静地离线跑：本地按键、屏幕、蓝牙都正常，只是不上云。
- 想换 MQTT 服务器就改 `APP_MQTT_BROKER`，默认用的是公共测试服 `broker.emqx.io`。
- 不想用某个功能，把对应 `APP_xxx_ENABLE` 改成 `0` 即可。

### `board_config.h` —— 引脚和参数

引脚、舵机角度、阈值出厂值都在这里，数值和原 ESP-IDF 工程**一模一样**。常改的两处：

```cpp
// 窗户和门两路舵机的关位、开位角度（度）
#define BSP_SERVO_WINDOW_DOOR_CLOSED_DEG   30    // 关
#define BSP_SERVO_WINDOW_DOOR_OPEN_DEG     150   // 开
```

```cpp
// 窗帘那路舵机不要了，写死停用：1=接，0=不接
#define BSP_SERVO_CURTAIN_ENABLE 0
```

---

## 四、串口命令台（波特率 115200）

上传完在串口监视器里敲 `help`：

```text
status                  查看全部设备 + 传感器状态
on   <dev>              开设备
off  <dev>              关设备
toggle <dev>            翻转开关
open  <dev> / close <dev>   开合窗户、门
set  <dev> <0-100>      设备调档（灯亮度 / 风速 / 开合度）
color <led_xxx> <r> <g> <b>  记一个颜色（单色灯只记状态）
auto <on|off>           自动联动总开关
cfg                     查看自动联动阈值
cfg <key> <value>       改阈值
say  <voice_cmd>        模拟走一遍语音链路
help-voice              列出所有语音指令名
adkey                   看五键键盘现在的电压
keyscan <秒>            盯着键盘电压变化
test / test run / next / off   按键自检模式
help                    帮助
```

`<dev>` 取值：`led_living`、`led_kitchen`、`led_bedroom`、`led_bath`、`fan`、`window`、`door`、`curtain`、`all`。

几个例子：

```text
on led_living
set fan 60
open window
cfg temp_fan_on_c 30
say fan_on
```

### 板上按键和五键键盘

| 操作 | 干什么 |
|---|---|
| KEY2 单击 | 切自动联动 开/关 |
| KEY2 长按 2 秒 | 切风扇 |
| 五键 左(1) / 右(2) | 翻页 |
| 五键 上(3) / 下(4) | 选设备 |
| 五键 OK | 确认（设备页=开关选中的设备，自检页=进自检） |
| 五键 OK 长按 | 退出自检 |

---

## 五、接线

接线和原 ESP-IDF 工程**完全一样**，照 `../../docs/接线文档.md` 接就行。关键几行：

| 模块 | 接到 |
|---|---|
| OLED / AHT20 | SDA→GPIO8，SCL→GPIO9，VCC→3.3V |
| 光敏 AO | GPIO1（VCC→3.3V） |
| 雨滴 AO / DO | GPIO2 / GPIO12（VCC→3.3V） |
| 四路灯 S | 客厅 GPIO4、厨房 5、卧室 6、浴室 7（VCC→3.3V） |
| 舵机 窗户 / 门 | 信号 GPIO16 / GPIO17，红→独立 5V，棕黑→GND |
| 风扇 | 红→独立 5V，黑→MOS 管「输出−」；MOS 栅极/信号→GPIO18 |
| 按键 KEY2 | 一脚 GPIO11，另一脚 GND |
| 五位 AD 键盘 | OUT→GPIO10，VCC→3.3V，GND→GND |
| 语音 ASRPRO | TXD→GPIO21，RXD→GPIO47（★ 交叉），VCC→独立 5V |
| 板载状态灯 | 板载，GPIO48（v1.0 板是 GPIO38） |

三条供电铁律（和原工程一样）：
1. 会转的、会响的（舵机 / 风扇 / 语音模块喇叭）走**独立 5V**，不要从开发板取电；
2. 所有 GND 必须连在一起（共地）；
3. 舵机 / 风扇 / 语音模块的电源和 ESP32 的 GND 拧在一起。

### 风扇那一路还没买 MOS 管？

一颗 NPN 三极管也能顶（**S8050 / SS8050 / 2N2222 / S9013**，别用 2N3904）：

```text
独立5V ──┬── 风扇 红
         │
         └── 1N4148（反并，负极朝 5V）   ← 续流，保护三极管
风扇 黑 ──┬── C（集电极）
          │
GPIO18 ─[1kΩ]── B
          │
GND ──┬─── E（发射极）
      └──[10kΩ]── B      ← 开机默认关，必加
```

基极电流约 2.6mA，SS8050 放大一百倍以上，带 0.2A 的风扇稳稳的；三极管会有 0.2~0.3V 压降，转速略降一点点。**代码不用改**，`fan_*` 那套逻辑是"高电平=开"。

---

## 六、和 ESP-IDF 版有哪几处不一样

| 项 | ESP-IDF 版 | 这个 Arduino 版 |
|---|---|---|
| 语音 | INMP441 麦克风 + ESP-SR 离线识别（闭源库） | **ASRPRO 串口模块**（GPIO21/47）。GPIO13/39/40 空着了 |
| 屏幕 | 自绘 GUI 框架 astra，5 个页面 | U8g2 画的 5 个页面，内容一样（主页/设备/传感器/自检/联网） |
| 灯 | 同一套（普通单色 LED 模块 + PWM 调光） | 一样 |
| 窗帘 | 写死停用 | 一样写死停用 |
| 阈值存储 | NVS | NVS（Arduino 的 Preferences，存同一套结构） |
| 上报 | MQTT topic 与 JSON 格式 | **完全一样**，手机 App 和 MQTTX 不用改 |
| 亮度单位 | 支持 BH1750 时是真 lux | 只有光敏电阻，`light_pct` 是百分比、没有 lux 字段 |
| 命令台 | 有 log / mic / voice-test 等调试命令 | 没有（那些是 ESP-IDF 专有的），其余命令都在 |

### 三个已知限制（原工程也有，或者受手机端限制）

1. **蓝牙的 state 报文可能发不出去**：报文约 400~500 字节，比 BLE 单包（MTU−3，手机一般协商到 244 字节）大，固件就**不发这一条**并打一句警告——原 ESP-IDF 版行为完全一样（现场日志里就有 `state 帧 447 字节 > 当前 MTU 23 的净载荷 20 字节，本条不上报`）。
   影响：手机 App 上"设备开关状态"不会实时回读（**下发命令是好的**，灯、风扇、门窗都能控）。要彻底解决得改手机端：在 `BleManager.onCharacteristicChanged` 里按类型码做跨通知拼包，或者让固件把 state 报文压到 244 字节以内。
2. **离线语音识别没有**：INMP441 + ESP-SR 是乐鑫的闭源库，Arduino 里没有，所以语音走 ASRPRO 串口模块（模块自己认字，认出来往串口发一行）。
3. **`light_pct` 是百分比不是 lux**：光敏电阻只能给相对亮度，原工程挂 BH1750 才有真 lux；这版按百分比走，光照联动阈值（默认 50 / 200）沿用原工程的值，现场如果觉得不对就按实际读数调。

文件对照，想改哪块就找哪个文件：

| 这个文件 | 对应原工程 |
|---|---|
| `esp32_smart_home.ino` | `main/main.c`（主循环 + 命令台 + 按键接线） |
| `board_config.h` | `components/BSP/board_config.h` |
| `app_config.h` | `sdkconfig.defaults` + `components/App/wifi_config.h` |
| `device_model.cpp/.h` | `components/App/device_model.c` |
| `automation.cpp/.h` | `components/App/automation.c` |
| `app_cmd.cpp/.h` | `components/App/app_cmd.h` + `mqtt_app.c` 里的命令分发 |
| `led.cpp` `fan.cpp` `servo.cpp` | `components/BSP/LED` `FAN` `SERVO` |
| `sensor.cpp` | `components/BSP/SENSOR` |
| `input.cpp` | `components/BSP/KEY` + `components/BSP/ADKEY` |
| `oled_ui.cpp` | `components/BSP/OLED` + `main/astra_glue.cpp` 的页面 |
| `voice.cpp` | `components/BSP/VOICE/voice.c` 的串口那条路 |
| `net_mqtt.cpp` | `components/App/wifi_sta.c` + `mqtt_app.c` + `mqtt_protocol.c` |
| `net_ble.cpp` | `components/App/ble_app.c` |
| `selftest.cpp` | `components/App/selftest.c` |
| `pwm_compat.h` | 原工程没有，这是为兼容 Arduino 核心包 2.x/3.x 加的 |

---

## 七、常见问题

**1. 编译报库相关的错**
- 报 `U8g2lib.h / PubSubClient.h / NimBLEDevice.h: No such file or directory`，或者报 `屏幕功能开着，但没找到 U8g2lib.h` 这种中文提示 → 库没装：按第二节装上，或者把 `app_config.h` 里对应的 `APP_xxx_ENABLE` 改成 `0`。
- 报一堆 NimBLE 内部的错（`NimBLEDevice.h` 能打开但里面的类型/函数找不到）→ **NimBLE-Arduino 版本和核心包不配对**：核心包 2.x 要装 NimBLE-Arduino **1.4.x**，核心包 3.x 要装 **2.x**。

**2. 提示找不到开发板 / 没有 ESP32S3 Dev Module**
开发板管理器网址没填对，或者 esp32 包没装完。

**3. 上传失败，报 `MD5 of file does not match` 或超时**
这块板子有这个毛病（原 ESP-IDF 工程每次也都得先全片擦除）。Arduino IDE 里这么办：
- `工具` → **Erase All Flash Before Sketch Upload** → 选 **Enabled**，再上传；
- 或者先把 Upload Speed 降到 **115200**；
- 实在不行换根 USB 线 / 换个 USB 口（供电不足也会这样）。

**4. 串口监视器里中文是乱码**
监视器右下角把波特率设成 **115200**；还乱就把换行符设成「NL 和 CR」。

**5. 屏幕不亮**
先看开机日志里 `功能开关: 屏幕=` 是不是「关」——那就是没装 U8g2。装了还黑屏就量一下 OLED 的 VCC 是不是 3.3V、SDA/SCL 有没有接反。

**6. 舵机方向反了 / 顶住不动**
改 `board_config.h` 里的两个角度：`BSP_SERVO_WINDOW_DOOR_CLOSED_DEG`（关位）和 `..._OPEN_DEG`（开位）。默认关 30°、开 150°，两头各留了余量，不会顶死。舵机抖或者"滋滋"响就把 `BSP_SERVO_MAX_PULSE_US` 从 2500 调到 2400。

**7. 风扇不转**
`set fan 100` 之后量 GPIO18 对 GND 应该是 3.3V。有电压但风扇不转，就是开关器件（MOS 管/三极管）或共地的问题；风扇那路的 5V 必须来自独立电源，不能从开发板 5V 取。

**8. 蓝牙连不上**
装了 NimBLE-Arduino 吗？开机日志里 `蓝牙=` 是不是「开」？手机 App 认的名字是 `app_config.h` 里的 `APP_BLE_NAME`（默认 `ESP32-SmartHome`）。

**9. 温度/湿度一直显示 `--`**
AHT20 没接好，或者 SDA/SCL 接反了。开机日志里 I2C 扫描会打印找到了哪些器件，正常应该看到 `0x38`（AHT20）和 `0x3C`（OLED）。

**10. 手机 App 上的窗帘点了没反应**
窗帘舵机是**故意停用**的（`BSP_SERVO_CURTAIN_ENABLE = 0`），命令会被拒掉、状态也不会假装变化。要用就改成 1，并把舵机接到 GPIO15。
