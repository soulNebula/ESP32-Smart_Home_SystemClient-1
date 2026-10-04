# 09 · BLE 协议（手机 App ↔ ESP32-S3 蓝牙直连）

> 本文是**手机蓝牙直连**这条链路的协议真源，与 [`05-MQTT协议.md`](05-MQTT协议.md) 并列。
> 两者**并存**：MQTT 走 WiFi（可远程），BLE 走蓝牙（本地直连，~10m，不需要路由器）。
> 两条链路喂的是**同一套 JSON**，最终都汇聚到 `device_model`。

---

## 0. ⚠️ 为什么是 BLE，而不是"蓝牙串口"（先看这条，能省几小时）

**ESP32-S3 硬件不支持蓝牙经典（BR/EDR），只支持 BLE（蓝牙低功耗）。**

这在芯片的 SOC 能力定义里就能确认：

| 芯片 | `soc_caps.h` |
|------|--------------|
| `esp32`（原版） | `#define SOC_BT_CLASSIC_SUPPORTED (1)` |
| **`esp32s3`（本工程）** | **连 `CLASSIC` 相关的宏都不存在** |

后果：**所有基于 SPP（Serial Port Profile）的方案都不能用**。网上大量"ESP32 蓝牙串口助手 / 蓝牙透传"教程走的是 SPP，在 ESP32-S3 上根本无法工作。

所以本工程用 **BLE + GATT 自定义服务**：手机连上来后，往一个特征写 JSON、从另一个特征收通知。这不是"退而求其次"，而是唯一可行的路。

---

## 1. 与 MQTT 的关系：同一套 JSON，多一条传输

改造后的分层：

```text
       语音 / 按键 / 自动联动 / 串口调试台
                     │
                     ▼
              device_model（唯一控制汇聚点）
                     ▲
                     │  app_cmd_handle_json()  ← 共用解析
        ┌────────────┴────────────┐
        │                         │
   mqtt_app.c                ble_app.c
   （注册为一条链路）          （注册为一条链路）
        │                         │
        └────────► app_link ◄─────┘
              拼 JSON + 广播
```

- **下行**：两条链路都调用同一个 `app_cmd_handle_json()` / `app_cmd_handle_config_json()`，所以命令语义**不可能漂移**。
- **上行**：`mqtt_publish_xxx()` 现在是"拼 JSON + `app_link_broadcast()`"，广播给所有已连接链路。BLE 只要注册一下就自动获得全部上行，业务代码零改动。

**因此：BLE 的 JSON 内容与 MQTT 逐字节一致**，字段定义请直接看 [`05-MQTT协议.md`](05-MQTT协议.md)，本文不重复。

---

## 2. 广播与 GATT 结构

| 项 | 值 |
|---|---|
| 广播名 | `SmartHome-<uid>`，`<uid>` = MAC 后 3 字节，**与 MQTT topic 里的 ID 完全相同** |
| Service UUID | `a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d` |
| RX 特征（Write / Write-No-Rsp） | `a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e` —— **手机 → 板子** |
| TX 特征（Notify） | `a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f` —— **板子 → 手机** |
| CCCD | 标准 `00002902-0000-1000-8000-00805f9b34fb`（开通知用） |

> 手机端请按**名称前缀 `SmartHome-` 过滤**扫描结果，不要写死完整名字——`<uid>` 每块板子不同。

> ⚠️ NimBLE 的 `ble_uuid128_t.value[16]` 是**小端序**。写固件时 UUID 字节要倒过来：
> `a1b2c3d4-…-c5d` → `{0x5d,0x4c,0x3b,0x2a,0x1f,0x0e,0x9d,0x8c,0x7b,0x4a,0xf6,0xe5,0xd4,0xc3,0xb2,0xa1}`

---

## 3. 帧格式（双向一致）

```text
byte[0]   = 类型码
byte[1..] = UTF-8 的 JSON 文本（不含结尾 '\0'）
```

**为什么用独立的类型字节，而不是往 JSON 里加 `"type"` 字段？**
因为 MQTT 那边靠 **topic** 区分消息类型。如果为了 BLE 往 JSON 里塞字段，MQTT 的 JSON 就会跟着变，破坏已验证的功能和 `05-MQTT协议.md` 的文档。用帧头字节后，**MQTT 的 JSON 保持逐字节不变**。

### 下行（手机 → 板子）

| 类型码 | 含义 | 处理 |
|:---:|---|---|
| `0x01` | 控制命令 | `app_cmd_handle_json(json, len, SRC_BLE)` |
| `0x02` | 阈值配置 | `app_cmd_handle_config_json(json, len, SRC_BLE)` |
| `0x03` | 主动查询 | 立刻回一次 state + sensor（JSON 内容被忽略） |

### 上行（板子 → 手机）

类型码数值**与 `app_msg_type_t` 枚举一致**：

| 类型码 | 含义 | 对应 MQTT topic |
|:---:|---|---|
| `0x01` | state 全设备状态 | `<base>/state` |
| `0x02` | sensor 传感器 | `<base>/sensor` |
| `0x03` | ack 命令回执 | `<base>/ack` |
| `0x04` | event 按键/语音事件 | `<base>/event` |
| `0x05` | config 当前阈值 | `<base>/config` |

---

## 4. ⚠️ MTU：不协商就发不下状态

BLE 默认 ATT MTU 只有 **23 字节**，去掉 3 字节 ATT 头，**单包可用载荷只有 20 字节**。

而 state JSON 约 **250 字节**：

```json
{"led_living":{"power":true,"level":80,"r":255,"g":255,"b":255},
 "fan":{"power":false,"level":0}, ... ,"auto":true,"rssi":-52,
 "ip":"192.168.1.23","uptime":1234}
```

**所以手机端连接后必须 `requestMtu(517)`**（NimBLE 侧 preferred MTU 设为 512），拿到 `onMtuChanged` 成功回调后再开通知。否则通知会被截断，App 端解析必然失败。

App 侧已做处理：MTU 协商失败或超时会回退重试 247，仍失败则界面给出警告。

---

## 5. 连接流程

```text
手机                                    板子
 │ 扫描（按名字前缀 SmartHome- 过滤）
 │ ─────────────────────────────────────►  广播中
 │ 连接
 │ ─────────────────────────────────────►  onConnect：记录 conn_handle
 │ requestMtu(517)
 │ ─────────────────────────────────────►  ble_att_set_preferred_mtu(512)
 │ ◄─────────────────────────────────────  onMtuChanged
 │ discoverServices()
 │ ◄─────────────────────────────────────  返回 Service + RX + TX
 │ 写 CCCD = ENABLE_NOTIFICATION_VALUE
 │ ─────────────────────────────────────►  Notify 开启
 │ 主动发一帧 0x03（查询）
 │ ─────────────────────────────────────►
 │ ◄─────────────────────────────────────  0x01 state / 0x02 sensor / 0x05 config
 │
 │ 之后：0x01 下发命令，板子回 0x03 ack + 0x01 state
 │       传感器每 1~2s 推一帧 0x02
```

断开后板子会**重新开始广播**，手机可直接重连。

---

## 6. 与 WiFi 共存（重要）

ESP32-S3 的 **WiFi 和 BLE 共用同一个 2.4GHz 射频**，同时工作必须依赖软件共存（`SW_COEXIST`）。本工程两者同时开启：

- WiFi 连路由器（供 MQTT 远程控制）
- BLE 供手机本地直连

共存是支持的，但吞吐会互相影响——本工程的负载很低（传感器 1~2s 一条、命令是偶发），**实际完全够用**。

另外：开发板是**带天线座**的。**如果天线没接上，BLE 距离和稳定性会明显变差**，请先确认天线已插好。

---

## 7. 怎么测（不依赖自制 App）

想先用现成工具验证固件侧，可以用任意通用 BLE 调试 App（如 **nRF Connect**、**LightBlue**）：

1. 扫描，找到 `SmartHome-xxxxxx`
2. 连接，**先请求 MTU 517**（nRF Connect 里有 "Request MTU"）
3. 找到 Service `…c5d`，对 TX 特征 `…c5f` **开启 Notify**（点三个箭头图标）
4. 应该立刻收到几帧：[类型字节][JSON]
5. 对 RX 特征 `…c5e` **写**一帧：第一字节 `0x01`，后面跟 `{"dev":"led_living","action":"on"}`
   （nRF Connect 里选 "Write" → 输入 HEX/文本；注意先写类型字节 `01`）
6. 观察客厅灯亮、并收到 ack + state

---

## 8. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 扫描不到 `SmartHome-` | 固件没开 BLE（menuconfig 里 `启用蓝牙 BLE 控制链路`）；或天线没接导致信号太弱 |
| 连上但收不到 state | **没协商 MTU**（默认 20 字节发不下 250 字节的 JSON）；或没写 CCCD 开通知 |
| 通知收到的 JSON 开头多一个乱码字符 | 那是**帧头类型字节**，解析时要跳过 `byte[0]` |
| 下发命令没反应 | 检查帧第一字节是否为 `0x01`；JSON 是否完整；可用 nRF Connect 只写纯 JSON 前的类型字节确认 |
| 手机一连上 WiFi/MQTT 就不稳 | WiFi 与 BLE 射频共存，属正常；拉远路由器或减少传感器上报频率 |
| 关灯后马上又被自动打开 | 检查 `device_model.c` 的 `src_is_manual()` 是否包含 `SRC_BLE`——BLE 命令必须算"手动操作"，否则会被自动联动立刻覆盖 |

---

## 9. 相关代码位置

| 文件 | 作用 |
|---|---|
| [`components/App/ble_app.c`](../components/App/ble_app.c) / `.h` | NimBLE GATT 服务端、帧解析、注册为一条链路 |
| [`components/App/app_link.c`](../components/App/app_link.c) / `.h` | 传输链路注册表 + 广播（MQTT/BLE 共用） |
| [`components/App/app_cmd.h`](../components/App/app_cmd.h) | 共用命令解析入口（实现目前位于 `mqtt_app.c`） |
| [`components/App/device_model.c`](../components/App/device_model.c) | `src_is_manual()` 与来源名表（含 `SRC_BLE`） |
| `android/` | 自制 Android App（Kotlin + Jetpack Compose） |
