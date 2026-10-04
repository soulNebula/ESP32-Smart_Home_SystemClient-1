# 05 MQTT 协议

> 本文与 `components/App/mqtt_protocol.h`（字段名与 topic 规划）和 `components/App/mqtt_app.c`（实际收发实现）**严格一致**。
> 表里的 retain / QoS 是从 `esp_mqtt_client_publish()` 的实参逐个抄下来的，不是推测。

## 1. Topic 命名规则

```text
base = <CONFIG_APP_MQTT_TOPIC_PREFIX> / <唯一ID>
                │                          │
                │                          └─ 默认取 MAC 后 3 字节（小写十六进制，如 a1b2c3）
                │                             可被 CONFIG_APP_MQTT_UID_OVERRIDE 覆盖
                └─ menuconfig「Topic 前缀」，默认 "smarthome"
```

**具体例子**：前缀 `smarthome` + 唯一 ID `a1b2c3` → `base = smarthome/a1b2c3`，
于是 `state` = `smarthome/a1b2c3/state`、`cmd` = `smarthome/a1b2c3/cmd`（设备实际订阅通配 `smarthome/a1b2c3/cmd/#`），其余见下一节的完整总表。

**怎么知道自己的唯一 ID**：开机日志里有一行

```text
I (1234) mqtt_app: started: broker=mqtt://broker.emqx.io:1883 client_id=esp32sh-a1b2c3
I (1234) mqtt_app: topics: state=smarthome/a1b2c3/state  cmd=smarthome/a1b2c3/cmd/#  config=... get=...
```

`client_id` 固定为 `esp32sh-<唯一ID>`（保证公共 broker 上不互相顶掉）。
MAC 读不到时（WiFi 未 init 的极端情况）`wifi_get_mac_suffix()` 会退回读 eFuse；连 eFuse 都失败时用 `esp_random()` 兜底并打警告。

## 2. Topic 总表

| 方向 | Topic | 说明 | retain | QoS |
|------|-------|------|--------|-----|
| 上行 | `<base>/availability` | `"online"` / `"offline"` 纯文本（**不是 JSON**） | **是** | **1** |
| 上行 | `<base>/state` | 全设备状态 JSON | **是** | **1** |
| 上行 | `<base>/sensor` | 传感器数据 JSON。**周期上报**，周期 = `CONFIG_APP_MQTT_PUBLISH_SENSOR_MS`（menuconfig，默认 2000ms）；此外 MQTT 连接成功时、每次收到 `<base>/get` 时各补发一条 | 否 | **0** |
| 上行 | `<base>/ack` | 命令执行结果 | 否 | **1** |
| 上行 | `<base>/event` | 按键 / 语音触发的动作事件 | 否 | **1** |
| 上行 | `<base>/config` | 生效中的联动阈值配置 JSON | **是** | **1** |
| 下行 | `<base>/cmd`（订阅 `<base>/cmd/#`） | 控制命令 JSON | — | 订阅 QoS **1** |
| 下行 | `<base>/config` | 改阈值 / 开关自动模式 | — | 订阅 QoS **1** |
| 下行 | `<base>/get` | payload 任意 → 立刻回一次 `state` + `sensor` | — | 订阅 QoS **1** |

补充说明：

- **LWT 遗嘱**：`session.last_will` = topic `<base>/availability`、payload `offline`、QoS 1、retain 1。设备异常掉线时 broker 自动代发。
- **`<base>/config` 是双向的**：设备**订阅**它接收配置，同时也把当前生效的配置**发布**到同一个 topic（retain）。手机端订阅它就能随时看到设备当前的阈值。
  > ⚠ MQTT 3.1.1 没有 NoLocal 选项，部分 broker 会把消息回显给发布者本身，理论上会造成"配置反复刷屏"。
  > 固件里已经加了**回环保护**：`mqtt_app.c` 用 `s_last_pub_config` 记下自己最后发布的配置 JSON，
  > `handle_config()` 收到与之逐字相同的 payload 时直接忽略。若在自建 broker 上仍看到异常刷屏，
  > 把"订阅配置"与"上报配置"拆成两个 topic 是最彻底的改法。
- **连接成功时**设备会立刻：订阅上述 3 个下行 topic → 发 `availability=online` → 主动发一次 `state` 和 `sensor`。
- **下行 payload 上限 256 字节**（`RX_BUF_LEN`），超了会回 `{"action":"cmd","ok":false,"detail":"payload too long"}`。MQTT 客户端内部缓冲是 `cfg.buffer.size = 2048`，超长消息会被拆片，**拆片只处理第一片**（打警告并丢弃）。另外设备每收到一条命令，除了回 `ack`，还会补发一次 `state`（兜底）。

## 3. 下行命令 JSON 全表（发布到 `<base>/cmd`）

公共规则：

- `action` 必填且必须是字符串；`dev` 除了 `{"action":"auto"}` 之外都必填。
- `value` 缺省为 **100**；可以是数字（自动钳到 0~100），也可以是布尔（`true`→100，`false`→0）。
- `color` 的 `r/g/b` 缺省都是 **255**（白色），自动钳到 0~255。
- 键名来自 `mqtt_protocol.h` 的宏：`MQTT_KEY_DEV`="dev"、`MQTT_KEY_ACTION`="action"、`MQTT_KEY_VALUE`="value"、`MQTT_KEY_R`="r"、`MQTT_KEY_G`="g"、`MQTT_KEY_B`="b"。

| 命令 JSON | 预期效果 |
|-----------|----------|
| `{"dev":"led_living","action":"on"}` | 客厅灯带亮（沿用记住的颜色与亮度，出厂默认 255,255,255 @100%） |
| `{"dev":"led_living","action":"off"}` | 客厅灯带灭（亮度/颜色被保留，下次 `on` 沿用） |
| `{"dev":"led_bedroom","action":"toggle"}` | 卧室灯带状态翻转 |
| `{"dev":"led_living","action":"set","value":60}` | 客厅灯带亮度 60%，并且**会自动点亮**（`value>0` 即视为开） |
| `{"dev":"led_living","action":"color","r":255,"g":0,"b":0}` | 客厅灯带设为纯红。**注意：`color` 只改颜色，不会把灯点亮**，通常先 `on` |
| `{"dev":"fan","action":"on"}` | 风扇 100% 转速（≈ `set value=100`） |
| `{"dev":"fan","action":"off"}` | 风扇停转（真正输出 0 占空比） |
| `{"dev":"fan","action":"set","value":75}` | 风扇 75% 转速 |
| `{"dev":"window","action":"open"}` | 窗户舵机转到 100%（全开），等价 `{"dev":"window","action":"set","value":100}` |
| `{"dev":"window","action":"close"}` | 窗户舵机转到 0%（关窗），等价 `set value=0` |
| `{"dev":"door","action":"open"}` | 门舵机转到 100%（开门） |
| `{"dev":"curtain","action":"set","value":40}` | 窗帘舵机停在 40% 位置（`>=50` 才算"开"） |
| `{"dev":"all","action":"off"}` | **全部关闭**：4 路灯灭、风扇停、3 路舵机归位到 0%（走 `device_all_off(SRC_MQTT)`） |
| `{"dev":"all","action":"on"}` | **4 路灯亮 + 风扇 100%**。刻意**不**去开窗/门/窗帘（把门"全开"是安全隐患）；要开合门窗请用单设备命令。串口调试台的 `on all` 与此语义完全一致 |
| `{"action":"auto","value":true}` | 打开自动联动（**不需要 `dev`**）。会回 `ack` + 重新发布 `config` |
| `{"action":"auto","value":false}` | 关闭自动联动（`value` 用数字时非 0 即为真） |

> `dev` 取值：`led_living` / `led_kitchen` / `led_bedroom` / `led_bath` / `fan` / `window` / `door` / `curtain` / `all`。
> `all` **只支持 `on` / `off`**，其它 action 会回 `{"action":"set","ok":false,"detail":"all: only on/off"}`。

### 3.1 错误应答一览

| 情况 | ack payload |
|------|-------------|
| JSON 解析失败 | `{"action":"cmd","ok":false,"detail":"bad json"}` |
| payload 为空或 >256 字节 | `{"action":"cmd","ok":false,"detail":"payload too long"}` |
| 缺 `action` 或不是字符串 | `{"action":"cmd","ok":false,"detail":"missing action"}` |
| 非 `auto` 命令缺 `dev` | `{"action":"cmd","ok":false,"detail":"missing dev"}` |
| `dev` 不认识 | `{"action":"on","ok":false,"detail":"unknown dev"}` |
| `action` 不认识 | `{"action":"xxx","ok":false,"detail":"unsupported action"}` |
| 执行失败（例如给风扇发 `color`） | `{"action":"color","ok":false,"detail":"ESP_ERR_INVALID_ARG"}` |
| 成功 | `{"action":"on","ok":true,"detail":"led_living"}`（`detail` 就是设备名） |

## 4. 下行配置 JSON 全表（发布到 `<base>/config`）

payload 是一个 JSON 对象，**可以只放你想改的键**，其余保持不动。布尔项可以直接写 `true`/`false`，也可以写 `1`/`0`。

| 键 | 类型 | 取值范围 | 默认值 | 说明 |
|----|------|----------|--------|------|
| `enabled` | bool | true / false | `true`（`CONFIG_APP_AUTO_ENABLE_DEFAULT=y`） | 自动联动总开关 |
| `light_on_lux` | number | **> 0** | `BSP_DEF_LIGHT_ON_LUX` = **50** | 光照低于此值 → 开客厅灯 + 合窗帘 |
| `light_off_lux` | number | **> light_on_lux** | `BSP_DEF_LIGHT_OFF_LUX` = **200** | 光照高于此值 → 关客厅灯 + 开窗帘 |
| `temp_fan_on_c` | number | -10 ~ 60 | `BSP_DEF_TEMP_FAN_ON_C` = **28** | 温度高于此值 → 开风扇（按 `fan_auto_speed`） |
| `temp_fan_off_c` | number | **< temp_fan_on_c** | `BSP_DEF_TEMP_FAN_OFF_C` = **26** | 温度低于此值 → 关风扇 |
| `rain_pct` | number | 0 ~ 100 | `BSP_DEF_RAIN_PCT` = **30** | 雨滴湿度高于此值 → 判下雨，关窗 |
| `fan_auto_speed` | number | 0 ~ 100 | **70** | 自动开风扇时的转速 %（四舍五入到整数） |
| `auto_light_enable` | bool | true / false | `true` | 是否启用光照联动 |
| `auto_temp_enable` | bool | true / false | `true` | 是否启用温度联动 |
| `auto_rain_enable` | bool | true / false | `true` | 是否启用雨滴联动 |
| `auto_window_reopen` | bool | true / false | `false` | 雨停后是否自动重开窗（默认不自动开，安全起见） |

**交叉约束**（由 `automation.c::automation_set_threshold()` 强制执行）：

- `light_off_lux` **必须严格大于** `light_on_lux`，否则拒绝（`ESP_ERR_INVALID_SIZE`）；
- `temp_fan_off_c` **必须严格小于** `temp_fan_on_c`，否则拒绝；
- `light_on_lux` 只要求 `> 0`；`temp_fan_on_c` 只要求落在 -10~60。

**取值范围之外的键**：未知键返回 `ESP_ERR_INVALID_ARG` 被拒；数值越界（含 NaN/Inf）返回 `ESP_ERR_INVALID_SIZE` 被拒。被拒的键**保持原值**，其余键照常生效。

### ★ 手机端发 JSON 的键顺序问题（务必注意）

`automation_set_threshold()` 是**逐个键即时校验**的，校验时会拿**当前**的另一个键做比较。
所以同一帧里想同时改「上限」和「下限」时，**键的先后顺序会决定成败**（cJSON 按 JSON 文本里的出现顺序遍历）：

| 目标 | ✅ 建议顺序（一定成功） | ❌ 反顺序（会被部分拒绝） |
|------|------------------------|--------------------------|
| 光照 50/200 → 30/40 | `{"light_on_lux":30,"light_off_lux":40}` | `{"light_off_lux":40,"light_on_lux":30}` → `40` 不 > 当前 `light_on_lux`(50)，**被拒** |
| 温度 28/26 → 30/29 | `{"temp_fan_on_c":30,"temp_fan_off_c":29}` | `{"temp_fan_off_c":29,"temp_fan_on_c":30}` → `29` 不 < 当前 `temp_fan_on_c`(28)，**被拒** |

**一句话规则**：**先发 `light_on_lux` / `temp_fan_on_c`（`*_on` 那个键），再发对应的 `*_off` 键**。
因为 `*_off` 的校验要参照 `*_on`，而 `*_on` 不参照任何键 —— 按这个顺序，只要你的目标组合本身合法，就一定一次成功。

配置帧处理完设备会：`automation_save()` 落盘 NVS → 发布最新的 `config` → 回 `ack`（`detail` 为 `"applied"` 或 `"partly applied"`）。

## 5. 上行 JSON 完整示例

### 5.1 `<base>/state`（retain，QoS1）

```json
{"led_living":{"power":false,"level":100,"r":255,"g":255,"b":255},
 "led_kitchen":{"power":false,"level":100,"r":255,"g":255,"b":255},
 "led_bedroom":{"power":false,"level":100,"r":255,"g":255,"b":255},
 "led_bath":{"power":false,"level":100,"r":255,"g":255,"b":255},
 "fan":{"power":false,"level":0},
 "window":{"power":false,"level":0},
 "door":{"power":false,"level":0},
 "curtain":{"power":false,"level":0},
 "auto":true,"rssi":-52,"ip":"192.168.1.23","uptime":1234}
```

- **只有 LED 才带 `r`/`g`/`b`**；风扇和舵机只有 `power` + `level`。
- `level` 对灯是亮度、对风扇是转速、对舵机是开合位置；对舵机 `level >= 50` 时 `power=true`。
- **LED 默认 `level` 是 100**（即使 `power=false`），因为关灯保留亮度。
- `auto` / `rssi` / `ip` / `uptime` 由 `mqtt_publish_state()` 追加；`rssi` 未连接时为 0，`ip` 未连接时为 `"0.0.0.0"`。

### 5.2 `<base>/sensor`（不 retain，QoS0）

```json
{"temp":26.5,"humi":58.2,"temp_valid":true,
 "lux":123.4,"light_pct":48.0,"light_is_bh1750":true,
 "rain":12.5,"rain_detected":false}
```

| 字段 | 含义 |
|------|------|
| `temp` / `humi` / `temp_valid` | 摄氏度 / 相对湿度 %RH / SHT30 或 AHT20 是否在线。`temp_valid=false` 时温度联动自动停用，`temp`/`humi` 保留上次值 |
| `lux` | BH1750 模式=**真实 lux**；光敏电阻模式=按平方曲线折算的**等效 lux**（0~500） |
| `light_pct` / `light_is_bh1750` | `light_pct`：0~100，越大越亮（两种模式都有）；`light_is_bh1750`：true=BH1750 真实 lux，false=光敏电阻折算 |
| `rain` | 0~100，越大越湿 |
| `rain_detected` | 传感器按**固定**阈值 `BSP_DEF_RAIN_PCT` 的初判；**真正的联动阈值在 `automation` 里**，以 `rain` 字段为准 |

### 5.3 `<base>/ack`（不 retain，QoS1）

```json
{"action":"on","ok":true,"detail":"led_living"}
{"action":"config","ok":true,"detail":"applied"}
{"action":"cmd","ok":false,"detail":"bad json"}
```

字段：`action`（被处理的 action 名，解析失败时固定为 `"cmd"` / `"config"`）、`ok`（bool）、`detail`（字符串，成功时通常是设备名或 `applied`）。

### 5.4 `<base>/event`（不 retain，QoS1）

```json
{"event":"key1_click"}
```

`event` 的取值（源码中实际会发出的）：

| 来源 | event 值 |
|------|----------|
| KEY1 单击 | `key1_click` |
| KEY1 长按 | `key1_long_all_off` |
| KEY2 单击 | `key2_auto_on` / `key2_auto_off` |
| KEY2 长按 | `key2_long_fan_toggle` |
| 语音 / 串口 `say` | 语音指令名，如 `led_living_on`、`fan_on`、`query_all`、`auto_off` |

### 5.5 `<base>/availability`（retain，QoS1）

纯文本（**不是 JSON**）：`online`（连接成功后主动发布）/ `offline`（LWT 遗嘱，设备异常掉线时由 broker 代发）。

## 6. 手机端 MQTTX 实测步骤

1. **装 MQTTX**（手机应用商店或 PC 客户端，EMQ 官方免费）。
2. **新建连接**：Host `broker.emqx.io`，Port `1883`，用户名/密码**留空**，Client ID 随便填（如 `mqttx-test-001`）。点 Connect。
3. **订阅上行 topic**（QoS 选 1）：先订通配，一条就能看全部设备 —— `smarthome/+/state`、`smarthome/+/sensor`、`smarthome/+/ack`、`smarthome/+/event`、`smarthome/+/availability`；想只看自己那台就把 `+` 换成具体 ID：`smarthome/a1b2c3/state`。
4. **确认能收到**：ESP32 一连上就会发 retained 的 `state` 和一条 `sensor`，之后 `sensor` 会按 `CONFIG_APP_MQTT_PUBLISH_SENSOR_MS`（默认 2s）**持续推过来**；也可以随时按第 6 步发一次 `/get` 主动要一条。
5. **发命令**：发布到 `smarthome/a1b2c3/cmd`，payload 填

   ```json
   {"dev":"led_living","action":"on"}
   ```

   预期：客厅灯带亮，`ack` 收到 `{"action":"on","ok":true,"detail":"led_living"}`，`state` 里 `led_living.power` 变成 `true`。
6. **主动查询**：发布任意内容（例如 `?`）到 `smarthome/a1b2c3/get`，立刻收到一条 `state` + 一条 `sensor`。
7. **改阈值**：发布到 `smarthome/a1b2c3/config`

   ```json
   {"light_on_lux":30,"light_off_lux":40}
   ```

   预期：`ack` 为 `{"action":"config","ok":true,"detail":"applied"}`，随后 `config` topic 上能看到最新的完整配置 JSON。
8. **验证离线通告**：给 ESP32 断电，几秒后 `smarthome/a1b2c3/availability` 应变成 `offline`（LWT 生效）；重新上电变回 `online`。

**排错提示**：收不到任何消息时，先回串口 `status` 看 `MQTT=OK` 还是 `down`；再核对 topic 里的唯一 ID 是否与你订阅的一致（`smarthome` 是默认前缀，改过 `CONFIG_APP_MQTT_TOPIC_PREFIX` 则 topic 跟着变）。另外建议把前缀改成带个人标识的字符串（如 `smarthome-zhangsan`），公共 broker 上重名前缀会互相看到对方设备。

## 7. 切换 broker

| 方式 | 操作 |
|------|------|
| **公共测试 broker（默认）** | menuconfig → `MQTT 设置 → Broker 地址` = `mqtt://broker.emqx.io:1883`（或 `mqtt://test.mosquitto.org:1883`），用户名/密码留空 |
| **自建 Mosquitto（局域网）** | ① 在 PC/树莓派上装 Mosquitto；② `mosquitto.conf` 里加 `listener 1883` 和 `allow_anonymous true`（测试用）；③ 查 PC 的局域网 IP；④ menuconfig → `Broker 地址` = `mqtt://192.168.1.100:1883`（换成实际 IP）；⑤ 手机连**同一个** WiFi，MQTTX 里 Host 填同一个 IP、端口 1883 |

自建示例（测试用，**不要用于公网**）：

```conf
listener 1883
allow_anonymous true
```

其余可调项：`Broker 用户名/密码`（留空则**不设置**这两个字段）、`Topic 前缀`、`唯一ID`、`传感器上报周期`（默认 2000ms，范围 500~60000）、`Keepalive`（默认 60s，范围 10~300）。

## 8. 安全提醒（★ 正式部署必读）

1. **公共 broker 是明文的，任何人都能订阅。** `broker.emqx.io` / `test.mosquitto.org` 上的 topic 对所有互联网用户可见 —— 灯控指令、温湿度、家里的 IP（`state` 里的 `ip` 字段）都会被别人看到，别人也能给你的设备发命令。**只用于课堂演示与联调。**
2. **正式部署必须换自建 broker 或云平台，并启用 TLS（`mqtts://`）+ 认证**（用户名/密码或客户端证书），最好再加 topic ACL。
3. **改成 `mqtts://` 需要额外改动**（当前工程**没有**做）：`sdkconfig.defaults` 里已有 `CONFIG_MQTT_TRANSPORT_SSL=y`（SSL 传输已编译进来），端口要改成 `8883`；但 `mqtt_app.c` **只设置了 `cfg.broker.address.uri`**，`cfg.broker.verification.*` 全为空 —— 没有任何 CA / 证书 / CRT bundle，直接填 `mqtts://...` 会握手失败。需要：打开 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` → 在 `mqtt_app.c` 加 `cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;`（并 `#include "esp_crt_bundle.h"`）→ `components/App/CMakeLists.txt` 的 `REQUIRES` 补 `esp-tls`、`mbedtls`。自签名证书则用 `cfg.broker.verification.certificate` 填 PEM 或 `use_global_ca_store`。
4. **注意 `client_id` 唯一性**：本工程用 `esp32sh-<唯一ID>`；两台设备用了同一个 ID 会在 broker 上互相顶下线。
