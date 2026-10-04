# 13 · 语音模块 —— INMP441 + ESP-SR 离线识别（插上线就能测）

> 本文是**新增的第二套语音方案**，与原有的「ASRPRO 外部模块（UART）」**二选一**，默认仍是 ASRPRO，行为完全不变。
> 引脚真源：[`board_config.h`](../components/BSP/board_config.h)；孔位对照：[`10-扩展板接线对照表.md`](10-扩展板接线对照表.md)。
> 原有串口命令词方案见 `components/BSP/VOICE/voice.c` 的文件头注释。
>
> ★★ **2026-10-02 真机排错记录（三个隐藏 bug，全部已修）**：本文早期版本把"喂帧
> 199 帧/秒"当成"达标"写进了 6.6 节 —— 那是错的，199 帧/秒其实是**第一个 bug
> 的症状**（I²S 实际采样率是配置的 2 倍），唤醒词喊不响的真因就是它。
> 三个 bug 按发现顺序：① I²S 双时隙 2x 采样率（见 6.7）；② 直流跟踪器
> 右移截断导致 +135 假直流（见 6.8）；③ 唤醒后识别任务被喂帧任务饿死（见 6.9）。
> 修复后：NS 降噪**关闭**（见 6.10）、识别任务优先级提到 7、OLED 增加
> 语音助手弹窗提示（见 4.1）。

---

## 0. ★★ 先纠正一个会让接线彻底失败的误解：INMP441 走的是 I²S，**不是 I²C** ★★

很多人看到"麦克风模块"就本能地往 **SDA/SCL**（本工程的 `IO8`/`IO9`）上插 —— **那样一定没有任何反应，而且不会报错**。

| | I²C（OLED / SHT30 / BH1750 用的） | I²S（INMP441 用的） |
|---|---|---|
| 本质 | 一条总线挂多个**有地址的**从机 | 点对点**音频数据流**，**没有地址** |
| 信号线 | `SDA` + `SCL`（2 根） | `SCK` 位时钟 + `WS` 帧同步 + `SD` 数据（**3 根**） |
| 谁发时钟 | 主机发 SCL，从机只应答 | 主机（ESP32）发 SCK/WS，麦克风跟着节拍吐数据 |
| 怎么分左右声道 | —— | 靠 `WS` 电平高低，所以 **`L/R` 脚必须固定接一端** |
| 接错的后果 | 扫不到地址，日志会提示 | **静默**：程序正常、日志正常、就是永远没声音 |

> 一句话：**INMP441 的 3 根信号线 + 1 根 L/R 一根都不能少**，少了任意一根的表现都是"静音"，不是"报错"。

---

## 1. 六根线接到哪（照这张表插）

| INMP441 引脚 | 接到 | 扩展板上的丝印孔 | 说明 |
|---|---|---|---|
| **SCK**（位时钟 / BCLK） | `GPIO13` | **左排 `IO13`** | ESP32 产生的位时钟 |
| **WS**（帧同步 / LRCLK） | `GPIO39` | **右排 `IO39`** | 低=左声道，高=右声道 |
| **SD**（串行数据） | `GPIO40` | **右排 `IO40`** | 麦克风 → ESP32 的音频数据 |
| **L/R**（声道选择） | **`GND`** | ②排任意 `GND` | ★ **单麦必须固定接一端**；悬空会左右乱跳 → 无声 |
| **VDD** | **`3V3`** | ②排任意 `3V3` | ★ **绝对不要接 5V**，INMP441 是 3.3V 器件 |
| **GND** | `GND` | ②排任意 `GND` | 和上面 L/R 可以共用同一个 GND 孔 |

**为什么选这三个 GPIO**（不是随便挑的）：

* `IO13` / `IO39` / `IO40` 都在你手上那块扩展板的丝印上，接线不用数针脚；
* **不与任何已定义功能冲突**：
  * `IO38` 是 DevKitC-1 **v1.0** 的板载状态灯，工程文档明确写「别占」→ **不用**；
  * `IO12` 预留给「雨滴模块 DO」、`IO14` 预留给「风扇测速 TACH」→ 虽然当前都没接，但保留原用途更省心；
  * `IO41` / `IO42` 留作以后的传感器扩展。
* **代价**：`IO39~IO42` 是 USB-JTAG 脚，占用后**内置 JTAG 调试失效**。
  本工程用 UART 桥（`GPIO43/44`）做控制台和烧录，**JTAG 本来就没用**，可接受。
* 万一你的板子没引出 `IO39`/`IO40`：把 `board_config.h` 里的
  `BSP_I2S_MIC_WS_GPIO` 改 `GPIO_NUM_12`、`BSP_I2S_MIC_SD_GPIO` 改 `GPIO_NUM_14`
  重新编译即可（前提是雨滴 DO 和风扇测速确实不接）。

> ⚠️ **供电**：INMP441 从扩展板 ②排的 `3V3` 取电即可（约 1mA），**不要**接到 ③排的 `5V`。
> ⚠️ **不要**把它插到 DC 座旁边那个写着 `TX/RX` 的小排针上（那是 UART0，烧录/日志口）。

---

## 2. 说哪个唤醒词

| 项目 | 值 |
|---|---|
| **唤醒词模型** | `wn9s_nihaoxiaozhi` |
| **★ 你要喊的话** | **「你好小智」** |
| 唤醒词模型体积 | 约 123 KB（所有中文唤醒词里最小的一个） |

喊完唤醒词，串口日志会出现：

```
I (xxxxx) VOICE_SR: ★ 已唤醒（唤醒词=「你好小智」）—— 请说命令词，6 秒内有效
```

**然后再**说命令词（例如「打开客厅灯」）。6 秒内没说出命令词会自动回到休眠，需要**重新喊唤醒词**。

> 唤醒一次可以连说多条命令（2026-10-02 改）：每条命令命中后窗口顺延 6 秒，
> 期间继续说下一条即可；连续 6 秒没有命令才回到休眠。误触发风险低 ——
> 窗口只在真实唤醒+命中后顺延，且命令词表只有 25 条，电视/聊天声几乎不会连续命中。
>
> 想换唤醒词：`idf.py menuconfig` → `ESP Speech Recognition` →
> `Load Multiple Wake Words (WakeNet9s)`，三个候选：
> `wn9s_nihaoxiaozhi`（你好小智）/ `wn9s_hilexin`（Hi,乐鑫）/ `wn9s_hiesp`（Hi,ESP）。
> **只能勾一个**。换完重新编译烧录，本文的唤醒词也要同步改。

---

## 3. 完整命令词表（25 条，与原有 `voice_cmd_t` 一一对应）

> 命令词通过**运行时 API** 注册（`esp_mn_commands_add`），**不需要重新训练模型**。
> 表里的"拼音"是交给 MultiNet 的实际识别单元（MultiNet 的中文识别单元就是拼音音节），
> **你照着"中文命令词"那一列念就行**。

| # | 中文命令词（念这个） | 拼音（识别单元） | 对应 `voice_cmd_t` | 效果 |
|--:|---|---|---|---|
| 1 | 打开客厅灯 | `da kai ke ting deng` | `led_living_on` | 客厅灯亮 |
| 2 | 关闭客厅灯 | `guan bi ke ting deng` | `led_living_off` | 客厅灯灭 |
| 3 | 打开厨房灯 | `da kai chu fang deng` | `led_kitchen_on` | 厨房灯亮 |
| 4 | 关闭厨房灯 | `guan bi chu fang deng` | `led_kitchen_off` | 厨房灯灭 |
| 5 | 打开卧室灯 | `da kai wo shi deng` | `led_bedroom_on` | 卧室灯亮 |
| 6 | 关闭卧室灯 | `guan bi wo shi deng` | `led_bedroom_off` | 卧室灯灭 |
| 7 | 打开浴室灯 | `da kai yu shi deng` | `led_bath_on` | 浴室灯亮 |
| 8 | 关闭浴室灯 | `guan bi yu shi deng` | `led_bath_off` | 浴室灯灭 |
| 9 | 打开全部灯 | `da kai quan bu deng` | `led_all_on` | 4 路灯全亮 |
| 10 | 关闭全部灯 | `guan bi quan bu deng` | `led_all_off` | 4 路灯全灭 |
| 11 | 打开风扇 | `da kai feng shan` | `fan_on` | 风扇转 |
| 12 | 关闭风扇 | `guan bi feng shan` | `fan_off` | 风扇停 |
| 13 | 打开窗户 | `da kai chuang hu` | `window_open` | 窗户舵机开 |
| 14 | 关闭窗户 | `guan bi chuang hu` | `window_close` | 窗户舵机合 |
| 15 | 打开门 | `da kai men` | `door_open` | 门舵机开 |
| 16 | 关上门 | `guan shang men` | `door_close` | 门舵机关 |
| 17 | 拉开窗帘 | `la kai chuang lian` | `curtain_open` | 窗帘舵机开 |
| 18 | 拉上窗帘 | `la shang chuang lian` | `curtain_close` | 窗帘舵机合 |
| 19 | 温度多少 | `wen du duo shao` | `query_temp` | 播报温度 ⚠ 见下 |
| 20 | 湿度多少 | `shi du duo shao` | `query_humi` | 播报湿度 ⚠ |
| 21 | 光照多少 | `guang zhao duo shao` | `query_light` | 播报光照 ⚠ |
| 22 | 播报全部 | `bao bao quan bu` | `query_all` | 播报温湿度 ⚠ |
| 23 | 状态如何 | `zhuang tai ru he` | `query_status` | 播报温湿度 ⚠ |
| 24 | 打开自动 | `da kai zi dong` | `auto_on` | 开自动联动 |
| 25 | 关闭自动 | `guan bi zi dong` | `auto_off` | 关自动联动 |

### ⚠ 关于第 19~23 条（查询播报）——不会出声，这是正常的

INMP441 **只有麦克风，没有功放和喇叭**。所以：

* **第 1~18、24~25 条**（控制类）：**完全正常**，灯会真的亮、舵机会真的动；
* **第 19~23 条**（查询类）：**会正常走完整条业务链路** —— 日志打印、OLED 状态、
  MQTT 事件上报都会发生，但**不会有声音**。

**将来要做语音播报怎么办**：esp-sr 自带中文 TTS（`esp-tts/esp_tts_chinese`），
不需要另买模块，但需要**另加一路 I²S 功放（例如 MAX98357A）+ 一个小喇叭**，
把 TTS 出来的音频播出去。本次任务不涉及这部分硬件。

### 为什么有些说法刻意"绕开"了

MultiNet 是按音节匹配的，**前几个音节相同的命令词会互相抢**。所以：

* 窗帘用「**拉开/拉上**窗帘」而不是「打开/关闭窗帘」——
  「打开**窗**户」和「打开**窗**帘」前三个音节 `da kai chuang` 完全一样，容易误识别；
* 门用「关上**门**」（`guan shang men`）而不是「关闭门」，拉开与「关闭窗户/风扇」的音节距离；
* 浴室用「浴**室**」（`yu shi`）而不是「卫生间」，避免和「全部」（`quan bu`）混淆；
* 查询用「温度多少 / 湿度多少」而不是「现在温度 / 现在湿度」，
  因为「现**在温度**」和「现**在湿度**」前两个音节 `xian zai` 相同。

> 命令词条数（25 条）远低于模型上限（200 条）。**加词越少越准**，
> 以后要加请优先挑和现有词差异大的说法。

---

## 4. ★ 插上线就能测：三步验证法

### 4.0 ★ OLED 语音助手弹窗（2026-10-02 新增，测试不用再盯串口）

本方案没有喇叭，唤醒/识别成功的"反馈"由 **OLED 弹窗**承担：

| 事件 | 屏幕弹出 | 时长 |
|---|---|---|
| 唤醒词命中 | 「已唤醒，请说命令」 | 1.2 秒 |
| 命令词识别成功 | 「已识别:打开客厅灯」（显示中文命令词） | 1.2 秒 |
| 命令窗口超时 | 「已退出唤醒」 | 1.2 秒 |

实现：识别任务只写共享提示（`voice_esp_sr.h` 的 `g_voice_ui_note`），
astra UI 任务每帧轮询消费并调 `popInfo()` 弹窗 —— **不在识别任务里直接弹窗**，
因为弹窗动画是阻塞循环，会拖垮实时识别。`launcher.cpp` 的 `popInfo` 文字绘制
从 `drawEnglish` 改为 `drawChinese`（原实现走 u8g2_DrawStr 不支持中文，会乱码）。

### 第 0 步：编译烧录（打开开关）

```powershell
# 1) menuconfig 里确认这两个开关
#    智能家居 → 语音识别来源（二选一）→ [*] 改用板载 INMP441 I²S 麦克风 + ESP-SR
#    ESP Speech Recognition → Load Multiple Wake Words (WakeNet9s) → [*] 你好小智
#    ESP Speech Recognition → Chinese Speech Commands Model → [*] mn7_cn
#  （本工程的 sdkconfig 已经配好，一般不用动）

# 2) 编译
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Task build

# 3) ★ 烧录：本工程请用 tools\flash.ps1（原因见下面「烧录铁律」第 1 条）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM31
```

> ⚠️ 打开这个开关后需要**联网拉取 esp-sr 组件**（含预训练模型，压缩包约 170MB）。
> 拉取一次之后就会缓存到工程内的 `managed_components/`，之后可以离线编译。

#### ⚠️⚠️ 烧录三条铁律（搞错会得到"能跑但喊了没反应"甚至 boot loop 的板子）

| # | 铁律 | 为什么 |
|--:|---|---|
| 1 | **用 `tools\flash.ps1` 烧，不要直接用 `idf.py flash`** | 真机实测（2026-09-30）：本开发板上 esptool 的「按区域扇区擦除」**不可靠** —— 往**已经写过**的区域写 app 时，1.95MB 的 app 镜像会写坏，esptool 报 `MD5 of file does not match data in flash!`，随后板子永远 reboot：`E (421) esp_image: Checksum failed` → `No bootable app partitions`。实测 6/6 完全吻合：**先做全片 chip erase 的每一次都成功，没做的每一次都失败**（与波特率无关：460800/230400/115200 表现一致；关掉压缩也一样）。`flash.ps1` 会先全片擦除再写，失败还会自动回退重试。**注意这不是只改了分区表才有的事**，见第 3 条 |
| 2 | **必须整片烧，绝对不要用 `app-flash`** | `app-flash` **只烧应用分区，不会烧 `model` 分区**。用了它，固件是新的但 `srmodels.bin` 还是旧的（或空的）→ ESP-SR 在 `esp_srmodel_init("model")` 处失败，日志里 `model 分区里共有 0 个模型`。改完模型配置后尤其要注意 |
| 3 | **全片擦除会清掉 NVS**（WiFi 配网、`automation` 运行期配置）。WiFi 的 SSID/密码是**编译进固件**的（`sdkconfig`），擦除后仍会自动重连（实测两次全片擦除后都正常连上「洋」并拿到 `192.168.31.202`）；但 `automation` 的配置会回到默认值 | 免得以为"擦完怎么连不上了/配置怎么变了" |

**正确的烧录命令**：

```powershell
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM31
```

> `flash.ps1` 做的事：先 `erase_flash`（全片擦除），再用 `esptool write_flash
> --no-compress @flash_args` 写入 5 个镜像（bootloader / app / 分区表 /
> otadata / srmodels），最后校验 **5 条 `Hash of data verified`** 才算成功。
> 想强制擦除可以加 `-Erase`；失败时会自动带上擦除重试一次。
>
> 底层原因尚未定位到硅层（可能是该板 flash 的扇区擦除时序问题），但**流程是被
> 6 次实测验证过的**；本文档只声称已验证的部分。

想确认 `model` 分区真的被烧了，看烧录日志里应当有这一行（地址必须是 `0x820000`）：

```
write_flash ... 0x820000 build\srmodels\srmodels.bin
```

> 这条命令行里的 `-ExtraArgs '-p','COM31'` 有讲究：PowerShell 5.1 把数组
> 展开给原生程序时会用**逗号**而不是空格拼接，导致 idf.py 收到一个
> `-p,COM31` 的整串参数并报 `No such option: -p`。`tools/build.ps1` 里已经
> 加了一步"按逗号再拆一次"来修掉这个问题（见该文件第 4 节的注释）。

### 第 1 步：看开机日志（**不用说话**就能确认麦克风工作没有）

```powershell
powershell -ExecutionPolicy Bypass -File tools\monitor.ps1 -Port COM31 -Seconds 40 -Reset
```

要看到的关键行：

```
I (xxx) I2S_MIC: init ok: 16000Hz / 32bit slot / mono  SCK=GPIO13 WS=GPIO39 SD=GPIO40
I (xxx) I2S_MIC: INMP441 是 I²S【不是 I²C】: L/R->GND, VDD->3V3(不要5V); ...
I (xxx) BOARD:   [ OK ] i2s mic
I (xxx) BOARD:   [MIC ] INMP441 正在拾音：rms=xxx peak=xxx 峰峰值=xxx dBFS=-xx.x
I (xxx) VOICE_SR: model 分区里共有 N 个模型：
I (xxx) VOICE_SR:   [0] wn9s_nihaoxiaozhi
I (xxx) VOICE_SR:   [1] mn7_cn
I (xxx) VOICE_SR:   [2] fst
I (xxx) VOICE_SR: ★ 唤醒词模型 = wn9s_nihaoxiaozhi   要喊的词 = 「你好小智」
I (xxx) VOICE_SR: ★ 命令词模型 = mn7_cn
I (xxx) VOICE_SR: AFE 就绪：feed_chunksize=xxx 采样点（xx ms）/ 采样率 16000 Hz / 通道 1
I (xxx) VOICE_SR: 命令词注册完成：成功 25 / 失败 0，共 25 条
I (xxx) VOICE_SR: =========== ESP-SR 启动成功 ===========
I (xxx) VOICE_SR: ★ 请喊：「你好小智」，然后说命令词（如「打开客厅灯」）
```

**如果没接麦克风**，你会看到（这是设计好的行为，不是崩溃）：

```
I (xxx) BOARD:   [ OK ] i2s mic
W (xxx) BOARD:   [MIC ] 没读到有效音频（rms=0 peak=0）—— INMP441 可能没接。...
W (xxx) VOICE_SR: 读麦克风失败 50 次 (ESP_ERR_TIMEOUT)：麦克风可能没接，语音识别暂不可用
I (xxx) MAIN: got ip: ...          ← ★ WiFi/MQTT/BLE/按键/OLED 全部照常工作
```

### 第 2 步：串口敲 `mic` —— **不用说话**就能判断麦克风是否工作

这是本方案最重要的排错命令。它直接读 I²S 的实时电平：

```
> mic

---- INMP441 麦克风自检 ----
  接线提示：INMP441 是 I²S【不是 I²C】
      SCK=GPIO13   WS=GPIO39   SD=GPIO40
      L/R=GND（单麦必须固定接一端）   VDD=3V3（★不要接5V）
  ---- 实时电平 ----
  最近一次读取 : 320 个采样点（读满）
  rms          = 47   （安静房间几十~几百是正常底噪）
  peak         = 210   （对着麦克风说话应明显变大）
  峰峰值       = 380   （★是否在拾音主要看它：≈0 = 没在工作）
  直流偏置     = -12   （理想接近 0；很大说明是直流不是声音）
  dBFS         = -43.8
  累计读取次数 = 1532， 削顶采样 = 0
  就绪(采到有效音频) = 是 ★
  ---- 结论 ----
  【麦克风在工作】—— 对着它说话再敲一次 mic，peak 应明显变大
```

**判读方法（重要）**：

| 现象 | 结论 | 去查什么 |
|---|---|---|
| 整行全是 `0`，峰峰值 `0` | **没采到任何数据** | `VDD`/`GND` 有没有接；`SD` 是否插错孔 |
| `rms`/`peak` 有值，但**每次敲 `mic` 数字几乎一模一样**（峰峰值 ≈ 0） | **数据恒定不动** | `SCK` 和 `WS` 接反了；或 `L/R` 悬空 |
| 安静时 `rms` 几十~几百、峰峰值几百 | **正常工作** | 对着麦说话，`peak` 应明显变大（几千） |
| `削顶采样` 持续增长 | 信号过载 | 把 `i2s_mic.c` 的 `I2S_MIC_RAW_SHIFT` 从 `14` 改成 `16` |

想看连续波形统计（每 500ms 一行）：

```
> mic dump 3
  [mic] t=   0ms  min=  -420 max=   510 avg=   -12 peak=   510 clip=0
  [mic] t= 500ms  min=  -380 max=   640 avg=    -8 peak=   640 clip=0
  ...
  [mic] 结论: 【麦克风在工作】—— 现在对着麦克风说话，peak 应明显变大
```

### 第 3 步：说唤醒词 + 命令词

1. 对麦克风说：**「你好小智」**
   → 日志：`★ 已唤醒（唤醒词=「你好小智」）—— 请说命令词，6 秒内有效`
2. 紧接着说：**「打开客厅灯」**
   → 日志：
   ```
   I (xxx) VOICE_SR: 命令词命中: 「led_living_on」 (conf=0.987) -> led_living_on
   I (xxx) VOICE_SR: 识别原文: da kai ke ting deng
   I (xxx) voice: voice cmd: led_living_on
   I (xxx) MAIN: （MQTT 事件上报）
   ```
   → 客厅灯**真的亮了**。

不确定有哪些命令词时，串口敲：

```
> voice-test
```

会打印唤醒词 + 上面那张 25 条命令词表（中文 / 拼音 / 对应指令）。

---

## 5. 模型与分区（model 分区怎么来的）

### 分区表改动（`partitions-16MiB.csv`）

```
Name,     Type, SubType,  Offset,   Size,
nvs,        data, nvs,      0x9000,   0x6000,
otadata,    data, ota,      0xf000,   0x2000,
phy_init,   data, phy,      0x11000,  0x1000,
ota_0,      app,  ota_0,    0x20000,  0x400000,    4MB  应用
ota_1,      app,  ota_1,    0x420000, 0x400000,    4MB  ★保留双 OTA
model,      data, spiffs,   0x820000, 0x400000,    4MB  ★新增：ESP-SR 模型
storage,    data, spiffs,   0xC20000, 0x3E0000,  ≈3.875MB  ★从 7.875MB 缩小
```

**为什么这么切**：

* `storage`（原来是 7.875MB 的 spiffs）在代码里**完全没有被引用**
  （全工程搜不到 `esp_vfs_spiffs` / `SPIFFS` 的任何调用），是块纯占位的空地，
  **压缩它零成本**；
* `ota_0`/`ota_1` 虽然目前没有 `esp_ota_*` 调用，但"将来做产品要能远程升级"
  价值很高，**保留**；
* `model` 分区**必须叫 `model`**、类型必须是 `data` —— 这是 esp-sr 的硬要求：
  * 它的 `CMakeLists.txt` 在配置阶段用 `partition_table_get_partition_info(...
    "--partition-name model" ...)` 查这个分区，查到就把选中的模型打包成
    `build/srmodels/srmodels.bin` 并**在 `flash` 时自动烧进该分区**；
  * 运行时 `esp_srmodel_init("model")` 用
    `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, SUBTYPE_ANY, "model")`
    查找，再用 `esp_partition_mmap` **直接内存映射**读取
    （不走文件系统，所以启动很快）。

### 实际模型体积

| 模型 | 作用 | 体积 |
|---|---|---:|
| `wn9s_nihaoxiaozhi` | 唤醒词「你好小智」 | ≈ 123 KB |
| `mn7_cn` | 中文命令词（MultiNet7） | ≈ 2609 KB |
| `fst` | mn6/7 的 FST 词表（随 mn7 自动带上） | ≈ 9 KB |
| **原始文件合计** | | **≈ 2742 KB** |
| **★ 打包后 `srmodels.bin`** | **实测值** | **2,807,298 字节 = 2.68 MB** |

**这个 2.68MB 是实测数字**（`C:\esp32_smart_home\build\srmodels\srmodels.bin`），
占 4MB 的 `model` 分区 **68.5%**，**余量约 1.32MB**。

构建日志里还会有一行脚本自己算的推荐值：

```
Recommended model partition size: NNNNK
```

也直接看文件：`C:\esp32_smart_home\build\srmodels\srmodels.bin`。

### 降噪 / VAD 为什么用 WebRTC 版而不是 AI 版

`menuconfig → ESP Speech Recognition` 里还有两个模型可选：

| 模型 | 体积 | 本工程选择 |
|---|---:|---|
| `nsnet2`（AI 降噪 v2） | 330 KB | ❌ 未启用 |
| `vadnet1_medium`（AI 语音活动检测） | 281 KB | ❌ 未启用 |
| WebRTC 降噪 / VAD | 0（算法内置，不进分区） | ✅ **当前启用** |

**理由**：换成 AI 版后模型总量会涨到 ≈3353KB（4MB 分区仍放得下），
但它们的**运行内存和 CPU 开销也会同步上涨**，而这颗芯片同时还要跑
WiFi + BLE + MQTT + OLED。语音不是本工程唯一的负载，所以保守选了
资源占用小得多、久经验证的 WebRTC 版。

想换 AI 版：`menuconfig` → `Select noise suppression model` → `Deep noise
suppression v2 (nsnet2)`，以及 `Select voice activity detection` →
`vadnet1 medium`，重新编译即可，**分区不用改**。

### ⚠ 烧录：本板必须用 `tools\flash.ps1`（先全片擦除）

改分区表后当然要全片擦除；但**本开发板即便是平时改个代码，直接 `idf.py flash`
也会写坏 app**（`MD5 of file does not match data in flash!` → boot loop）。
原因和实测证据见第 4 章的「烧录三条铁律」。总之：

```powershell
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM31
```

---

## 6. 排错清单

### 6.1 「怎么喊都没反应」

按顺序查（**先敲 `mic`，这一步能排除掉一大半问题**）：

| # | 查什么 | 正常应该 | 不对怎么办 |
|--:|---|---|---|
| 1 | 串口敲 `mic` | `就绪 = 是 ★` | 见下面 6.2 |
| 2 | 日志有没有 `★ 唤醒词模型 = ... 要喊的词 = 「你好小智」` | 有 | 没有 → 6.3（模型没进分区） |
| 3 | 日志有没有 `命令词注册完成：成功 25 / 失败 0` | 25/0 | 失败数 >0 → 看紧邻的 `命令词注册失败：...` 日志 |
| 4 | 说唤醒词时有没有 `★ 已唤醒` | 有 | 没有 → 6.4（唤醒不了） |
| 5 | 唤醒之后 6 秒内说命令词了吗 | —— | 超时会打 `命令词等待超时`，**要重新喊唤醒词** |
| 6 | 命令词表里的说法念对了吗 | —— | 敲 `voice-test` 对照；注意第 3 节说的"刻意绕开" |

### 6.2 「`mic` 显示没采到音频」

| 检查项 | 说明 |
|---|---|
| `VDD` 接的是 **3V3** 吗 | 接 5V 会损坏器件，接错也不会工作 |
| `GND` 和 `L/R` 都接了吗 | **`L/R` 悬空 = 静默**，这是最常见的漏接 |
| `SD` 插的是 `IO40` 吗 | 插到 `IO39`（WS 的孔）就会数据错乱 |
| `SCK`/`WS` 有没有接反 | 表现是"数字恒定不动"，用 `mic dump 3` 看 `min`/`max` 是否一直相等 |
| 杜邦线是否虚接 | 换一根线 / 换同行的另一个孔（每行有红黄两孔，相通） |

### 6.3 「日志说找不到唤醒词模型 / model 分区里只有 4 字节」

说明 `srmodels.bin` 没生成（模型一个都没选中）或没烧进分区。查：

1. `sdkconfig` 里这几项：
   ```
   CONFIG_APP_VOICE_SOURCE_ESP_SR=y
   CONFIG_SR_WN_WN9S_NIHAOXIAOZHI=y          ← 唤醒词（至少一个 =y）
   CONFIG_SR_MN_CN_MULTINET7_QUANT=y         ← 命令词（至少一个 =y）
   # CONFIG_SR_MN_CN_NONE is not set          ← 这一项必须【不是】=y
   ```
   用 `Select-String sdkconfig -Pattern 'SR_WN|SR_MN_CN'` 一眼就能看出来。
2. 构建日志里有没有 `Recommended model partition size: NNNNK`；
3. `build\srmodels\srmodels.bin` 是不是 **4 字节**（4 字节 = 空的）；
4. 烧录是不是带了 `erase-flash` 并且是 `flash`（不是 `app-flash`
   —— `app-flash` **不会**烧模型分区）。

> 本工程在 `voice_esp_sr.c` 里加了**编译期检查**：如果打开 ESP-SR 开关
> 却一个唤醒词/命令词模型都没选，会直接**编译不过**，错误信息里写明该去哪个
> 菜单勾什么，避免"能编能烧但喊了没反应"这种最难查的情况。

### 6.4 「能测到麦克风电平，但唤醒不了 / 识别率低」

| 现象 | 可能原因与对策 |
|---|---|
| 喊很多次才醒一次 | 距离太远 / 环境太吵。INMP441 是**全向麦**，离嘴 **20~50cm** 效果最好，别贴着也别对着风扇/空调出风口 |
| 喊了没反应，但 `mic` 里说话 peak 很大 | 唤醒词说错（是「**你好小智**」）。也可能是语速/口音，试着放慢、吐字清楚 |
| 唤醒可以，命令词老识别错 | ① 命令词要说得**连贯**（"打开客厅灯"一口气，别一个字一个字蹦）；② 环境噪声大就考虑换 AI 降噪（见第 5 节）；③ 核对第 3 节里"刻意绕开"的那些说法 |
| 经常被电视/聊天**误唤醒** | 唤醒门限调高：`voice_esp_sr.c` 里 `afe_cfg->wakenet_mode = DET_MODE_90;` 改成 `DET_MODE_95`（更不容易误唤醒，但更难唤醒，需要权衡） |
| 说话时日志刷 `削顶 N 次` | 移位量偏大，把 `i2s_mic.c` 的 `I2S_MIC_RAW_SHIFT` 从 `14` 改成 `16` |
| 唤醒后 6 秒窗口太短 | 改 `voice_esp_sr.c` 的 `VOICE_SR_MN_DURATION_MS`（默认 6000） |

### 6.5 「开了 ESP-SR 之后 WiFi / MQTT / BLE 有问题？」

这是本任务最大的风险点，排查顺序：

1. 看开机日志里 `剩余内部堆 N 字节 / PSRAM 剩余 N 字节`（`VOICE_SR` 打的）。
   内部 RAM 剩 < 30KB 就要警惕。
2. ESP-SR 的 AFE 中间缓冲已经通过
   `memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE` 尽量放到 PSRAM，
   把内部 RAM 留给 WiFi/BLE 协议栈。
3. 识别任务绑在**核心 1**、优先级 `3`（低于按键扫描 4 和主循环 5），
   并且每轮都主动 `vTaskDelay(1)` 让出 CPU —— 就是为了不饿死无线任务。
4. 如果仍然不稳：先试 `log quiet` 关掉 BLE 日志减少串口占用；
   再考虑换更小的模型（`mn5q8_cn`，省约 0.5MB 且推理更轻）。

**实测结果（板子 + 真机联调）**：这三条无线链路在 ESP-SR 常驻监听下**全部正常**：

```
wifi:connected with 洋      →  sta ip: 192.168.31.202
app_link: link registered: "mqtt"(1)  "ble"(2)
mqtt_app: connected to broker.emqx.io
VOICE_SR: 剩余内部堆 203691 字节 / PSRAM 剩余 5206084 字节
```

> 内部 RAM 还剩 **约 199KB**、PSRAM 还剩 **约 5.0MB**，余量很充裕。

---

### 6.6 「串口被 `AFE: Ringbuffer of AFE is empty, Please use feed() to write data` 刷屏」

**这个缺陷在真机联调时出现过，已修复。记录在这里是因为它极具迷惑性 ——
而且我们为它先后给出过两个错误结论，最终靠实测数据才定案。**

| | |
|---|---|
| **症状** | 45 秒日志 6631 行，其中 **6274 行（94.5%）** 是这一条；串口调试台完全没法用（敲不进 `mic` / `voice-test` / `status`） |
| **迷惑点 1** | `mic` 命令打出的电平是**存在的**（`i2s_mic_read()` 本身没问题），容易误判成"I²S 配置错了"或"麦克风坏了" |
| **迷惑点 2** | 这条告警的文字本身就是误导：它写着 `Please use feed() to write data`，于是很容易认定"是喂帧那一侧的问题" |
| **真因（实测确认）** | 原代码用 `s_afe->fetch_with_delay(afe_data, **0**)` —— 超时给 0 等于**空轮询**。AFE 在环形缓冲还没攒够一帧时被问一次，就打一条这条告警。而本配置里 **`feed_chunksize=160` 但 `fetch_chunksize=512`**（相差 3.2 倍），所以"0 超时取一次"几乎**必然**问到空 → 每轮循环打一条，实测稳定 **155 行/秒**。又因为这条告警走 115200 串口是**写阻塞**的，它反过来拖慢循环，形成"越问越空、越空越打"的自激 |
| **修法** | ① 把"读 I²S + feed"和"fetch + 识别"**拆成两个任务**（官方 ESP-SR 示例的结构）。因为**阻塞式 fetch 必须有个专门的喂帧者**：识别任务一旦阻塞在 fetch 里，就没人读 I²S、没人 feed 了 —— 这正是原设计只能退化成空轮询的原因。② 识别任务改用**阻塞式 fetch**（超时 100 ms），等不到就安静返回 NULL，不再去"问空" AFE |
| **效果** | 同长度日志里这条告警 **4752 行 → 2 行**；剩下 2 行都出现在**喂帧任务启动之前**的启动窗口内（属正常）。串口调试台恢复可用 |
| **配套加固** | ① 所有告警（读失败 / feed 失败 / 全 0 帧）全部限速；② 状态日志改成 **5 秒一条**，并把它移到循环里**所有 `continue` 之前** —— 上一版这条日志写在 fetch 之后、fetch 一失败就 `continue`，结果**一次都没打出来**，排查时等于没有眼睛 |

#### 这个 bug 附带的价值：两个被推翻的错误结论（值得记住）

排查过程中先后给出过两个"听起来很合理"但**是错的**结论，最后都是靠**加计数器实测**推翻的：

| 错误结论 | 当时依据 | 为什么错 | 实测数据 |
|---|---|---|---|
| ①「一次 `i2s_mic_read()` 读不满就 `continue` 丢掉 → `feed()` 从来没被调用 → AFE 空」 | 代码里确实有"读不满就丢且不 sleep"的写法，看起来完全能解释刷屏 | **改完刷屏一行没少**（仍占 93%）。真因不在这里 | 累加修复后刷屏 4752 行，与修复前 6274 行同级 |
| ②「喂帧速率只有 62 帧/秒 < 目标 100 → 丢了 38% 音频 → 该提优先级/优化 I²S 读」 | 状态日志里写着"62 帧/秒，目标 100" | 那个 62 是 **fetch 成功次数**，不是喂帧次数！`fetch_chunksize(512)/feed_chunksize(160)=3.2`，而实测喂帧是 **199 帧/秒**，一直达标。两个不同的量被拿来对比，凭空造出一个不存在的问题 | 加计时探针后：feed **199 帧/秒**、占用 99%、读均 4.7ms、feed 均 0.3ms；`199/3.2 = 62` ✔ 完全自洽 |

> **教训一**：**"读到了数据"≠"数据被喂进去了"**，**"fetch 到的次数"≠"喂帧速率"**。
> 指标必须**把量纲和口径写清楚**，否则日志本身就会骗人 —— ②就是被自己打的日志骗了。
> 现在状态日志把两个速率**分开打、各自对标**：
> `feed N 帧/M 帧每秒（目标 100，每帧 160 点） | fetch N 次/M 次每秒（每次 512 点）`。
>
> **教训二**：只记录**均值**、不记录**次数**是不够的 —— 少了次数就算不出"占用率"，
> 当时因此还误判过一次"任务被抢占、69% 时间没跑"（实测占用 99%，根本没饥饿）。
> 关键路径上要同时埋"次数 + 耗时"。
>
> **教训三**：面对"底层库在刷屏"，先怀疑**自己是不是把 API 用错了**
> （这里就是把非阻塞 fetch 当轮询用），而不是先怀疑硬件或性能。
>
> **教训四（2026-10-02 补充）**：上面"实测喂帧 199 帧/秒 > 目标 100"这个
> 结论本身也被推翻了 —— 16KHz 麦克风的物理上限就是 100 帧/秒（160 点/帧），
> 199 帧/秒说明 I²S 交付的是 **2 倍数据**。当时的"达标"是拿一个物理上不可能
> 的速率当基准。**"指标达标"要先问"这个指标物理上该是多少"。**
> 真因与修法见 6.7。

### 6.7 ★★★ 「唤醒词怎么喊都不响」的真因：I²S 实际采样率是配置的 2 倍（2026-10-02）

| | |
|---|---|
| **症状** | 唤醒词「你好小智」喊几十遍只偶尔响一次；VAD 每次都能触发（慢放的声音也有能量），但 WakeNet 几乎从不命中 |
| **破案线索** | 状态日志里喂帧速率稳定 **199 帧/秒**（160 点/帧）。16KHz 麦克风的物理上限是 100 帧/秒 —— 199 帧/秒 = I²S 以 ≈32,000 字/秒 交付数据，配置明明是 16000 |
| **根因（读 IDF 5.4.4 源码证实）** | `esp_driver_i2s/i2s_std.c` 的 `i2s_std_set_slot()` 里 `handle->total_slot = 2;` 是**硬编码**的，与 slot_mode 无关。Philips 模式下每个 WS 周期固定两个 32bit 时隙：bclk = 16000×2×32 = 1.024MHz，WS = 16KHz（麦克风这边没问题），但 **DMA 按时隙速率 32,000 字/秒把两个时隙全部装进缓冲**。INMP441 的 L/R 接 GND 只在左时隙（WS 低）输出，右时隙 SD 脚悬空（实测读回恒定 ~0） |
| **后果** | 把 [左=麦克风][右=垃圾] 的交错流当成 16KHz 连续音频喂给 ESP-SR → 音频时间被拉伸 2 倍 → "你好小智"变成慢动作 → WakeNet 特征完全对不上。它同时解释了早期排错里的所有怪现象：199 帧/秒"达标"、+135 假直流（其实是 DC 跟踪器 bug，见 6.8）、波形不对称 |
| **修法** | `i2s_mic.c` 里读【2 倍原始字】，只取**偶数下标**（左时隙）做抽取，得到真正的 16KHz 单声道流。修复后状态日志恢复正常物理速率：**feed 31 帧/秒 × 512 点 = 16,000 采样/秒**（NS 关闭后 AFE 的 feed_chunksize 从 160 变成 512，31×512=16000，与 100×160=16000 等价） |
| **时隙奇偶** | 实测：偶数下标 = 有信号的时隙（底噪 ±200、峰峰值 200+）；奇数下标 = 悬空时隙（恒定 ~0，峰峰值 3）。中间一度误判翻转奇偶，翻完 mic 自检直接报"没采到音频"，翻回偶数恢复。**判断哪个时隙有信号：敲 `mic` 看峰峰值，≈0 的就是悬空时隙** |

> 注意：早期文档把 199 帧/秒写成"达标"（6.6 节教训二），是错的；
> 教训四里补充了为什么。硬件侧没有别的接法 —— INMP441 就是双时隙器件，
> 本修法是软件侧的正确处理。

### 6.8 直流偏置跟踪器的右移截断 bug（+135 假直流）

| | |
|---|---|
| **症状** | `mic` 自检里"直流偏置"恒为 **+128~150**（文档声称理想接近 0），波形 min 几乎全为正 |
| **根因** | `i2s_mic.c` 的直流跟踪器 `s_dc_est += (v - s_dc_est) >> 8;` —— C 的算术右移对负数**向下取整**，每步平均多减一点点，跟踪值被缓慢拽向负值（实测停在 ≈-135），等于给输出叠加 +135 假直流 |
| **修法** | `+128 再 >> 8` 变成 round-to-nearest：`s_dc_est += (v - s_dc_est + (1 << (SHIFT-1))) >> SHIFT`。修复后 `mic` 自检直流偏置 ≈ 0（实测 -27 ~ 0） |
| **教训** | 定点 IIR 的移位更新**必须四舍五入**，否则负号路径的截断偏差会积累成一个看不见的直流源 |

### 6.9 唤醒后命令词卡死：识别任务被喂帧任务饿死（2026-10-02）

| | |
|---|---|
| **症状** | 唤醒成功后，说命令词没反应；日志刷 `Ringbuffer of AFE(FEED) is full`；6 秒命令窗口 52 秒才走完（按音频时间计） |
| **数据** | 唤醒后 fetch 速率从 31~62 次/秒塌到 **2~3 次/秒**，喂帧任务 feed 均耗时从 2.5ms 涨到 9ms。两个任务都钉在核心 1：喂帧优先级 5 吃掉 ~95% CPU，识别任务（优先级 3）被饿死，MultiNet detect 每条帧要数百毫秒 |
| **修法** | 识别任务优先级 3 → **7**（高于喂帧 5）：识别时拿满 CPU，喂帧有 60ms I²S DMA + 500ms AFE 环形缓冲兜底。状态日志增加 `detect=N次 均=X.XXms` 探针 |

### 6.10 NS 降噪已关闭（2026-10-02）

原本启用 WebRTC NS（第 5 节的理由："保守选资源占用小的"）。真机实测发现：
esp-sr 开机就警告 `Noise Supression may reduce the accuracy of speech
recognition`，且 NS 开启时唤醒词几乎无法触发（10 余次喊话 0~1 次命中）。
**现已关闭 `ns_init`**（与官方 wakenet 示例的默认配置一致）；家居底噪由
VAD 门控 + wakenet 自身鲁棒性处理。第 5 节表格里"WebRTC 降噪/VAD ✅ 当前启用"
的降噪部分已过时，以本节为准。

---

## 7. ASRPRO 方案 ↔ ESP-SR 方案 对比

| | **ASRPRO（原有，默认）** | **INMP441 + ESP-SR（本文）** |
|---|---|---|
| 开关 | `CONFIG_APP_VOICE_SOURCE_ESP_SR = n` | `= y` |
| 硬件 | 外部 ASRPRO 模块 + 它的麦/喇叭 | 一颗 INMP441（约 10 元） |
| 接口 | UART1（`IO47` TX / `IO21` RX，9600 8N1） | I²S（`IO13`/`IO39`/`IO40`） |
| 唤醒词 | 由模块固件决定（需用天问Block 烧录） | **「你好小智」**（换词要改 menuconfig 重编） |
| 命令词 | 模块固件里配 | 代码里运行时注册（改 `voice_esp_sr.c` 的表） |
| 改命令词成本 | 要用上位机重新烧模块 | 改代码重新编译（**不用重新训练模型**） |
| 语音播报 | ✅ 模块自带喇叭 | ❌ 需另加 I²S 功放 + 喇叭 |
| 离线依赖 | 无（纯串口） | 需要联网拉一次 esp-sr（约 170MB） |
| 占用的 GPIO | `IO21` / `IO47` | `IO13` / `IO39` / `IO40`（后者牺牲内置 JTAG） |
| 上层业务代码 | —— **两者完全一样，一行都不用改** —— | |

> 两套方案的识别结果都会被翻译成同一个 `voice_cmd_t`，调用同一个
> `voice_dispatch()`，所以 `main.c::voice_on_cmd()` / MQTT / OLED / 自动联动
> **完全感知不到区别**。这就是当初把"语音抽象层"和"业务逻辑"解耦的收益。

---

## 8. 关于"完全无网"编译（重要，别踩）

本工程原本的原则是「零在线依赖，无网也能一次编译通过」。引入 ESP-SR 后**这条原则为语音让了一步**：

| 场景 | 能不能无网编译 |
|---|---|
| **关掉** `CONFIG_APP_VOICE_SOURCE_ESP_SR`（默认，用 ASRPRO） | ✅ **能**。CMake 不会去 REQUIRE esp-sr，`managed_components/` 为空也照样编译 |
| **打开** ESP-SR 开关 | ⚠️ **首次不能**。必须联网让组件管理器拉一次 esp-sr + esp-dsp + dl_fft（约 228MB） |

**拉过一次之后就离线可编了**，因为它落在工程内的 `managed_components/` 里。

> ⚠️ 但 `.gitignore` 默认把 `managed_components/` 和 `dependencies.lock` 排除了。
> 所以如果要在**完全无网**的机器上编译 ESP-SR 方案，需要二选一：
> 1. 把 `managed_components/` 和 `dependencies.lock` 也纳入版本管理
>    （注释掉 `.gitignore` 里那两行），一起拷贝过去；
> 2. 或者接受"那台机器上不启用 ESP-SR"。

依赖声明的来源：[`main/idf_component.yml`](../main/idf_component.yml)（含版本钉死 `2.3.1` 的理由）。

---

## 9. 相关文件索引

| 文件 | 作用 |
|---|---|
| [`components/BSP/I2S_MIC/i2s_mic.h`](../components/BSP/I2S_MIC/i2s_mic.h) | INMP441 驱动接口 + 接线说明 |
| [`components/BSP/I2S_MIC/i2s_mic.c`](../components/BSP/I2S_MIC/i2s_mic.c) | 新版 I²S 驱动、32bit 槽取高位、直流补偿、整数 RMS/dBFS |
| [`components/BSP/VOICE/voice_esp_sr.h`](../components/BSP/VOICE/voice_esp_sr.h) | ESP-SR 引擎接口 |
| [`components/BSP/VOICE/voice_esp_sr.c`](../components/BSP/VOICE/voice_esp_sr.c) | **命令词表在这里**、AFE/MultiNet 初始化、识别任务 |
| [`components/BSP/VOICE/voice.c`](../components/BSP/VOICE/voice.c) | 语音抽象层 + 来源切换（`voice_init()`） |
| [`components/BSP/VOICE/voice_internal.h`](../components/BSP/VOICE/voice_internal.h) | `voice_dispatch()`：两套来源的唯一汇合点 |
| [`components/BSP/board_config.h`](../components/BSP/board_config.h) | **引脚唯一真源**（第 9 节登记了 I²S 三根线） |
| [`partitions-16MiB.csv`](../partitions-16MiB.csv) | 分区表（含 `model` 分区与容量计算） |
| [`main/idf_component.yml`](../main/idf_component.yml) | esp-sr 依赖声明与"为什么要联网"的说明 |
| [`main/Kconfig.projbuild`](../main/Kconfig.projbuild) | 语音来源开关 + 模型选择菜单路径说明 |
| [`10-扩展板接线对照表.md`](10-扩展板接线对照表.md) | 丝印孔位 ↔ GPIO 对照（INMP441 三根线已登记） |
| [`../tools/voice_debug.py`](../tools/voice_debug.py) | ★ PC 本地调试器：vosk 离线中文复刻唤醒+命令词链路，输出与 MQTT 同格式的 JSON（`--say` 注入 / `--no-wake` / 模型自动下载） |
