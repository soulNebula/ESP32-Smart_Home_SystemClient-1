# SmartHome BLE — 原生 Android 控制端

ESP32-S3 全屋智能家居的安卓控制 App。原生 **Kotlin + Jetpack Compose**，通过 **BLE（蓝牙低功耗）**
GATT 与开发板通信，不使用任何第三方蓝牙库（只用 Android 自带的 `android.bluetooth.*`）。

| 项 | 值 |
|---|---|
| 包名 | `com.smarthome.ble`（debug 变体为 `com.smarthome.ble.debug`） |
| minSdk | 26（Android 8.0） |
| targetSdk / compileSdk | 35 |
| Kotlin | 2.0.21（含 `org.jetbrains.kotlin.plugin.compose`） |
| AGP | 8.7.0 |
| Gradle | 8.9 |
| JDK | 17 目标字节码；构建用 JDK 21（Android Studio JBR） |
| 依赖 | `compose-bom 2024.10.01`、`material3`、`foundation`、`ui`、`activity-compose`、`core-ktx`、`lifecycle-runtime-ktx` |

> 刻意**没有**引入 `material-icons-extended`：界面只用文字标签，而那个 AAR 有 35 MB，
> 在本机下载会长时间卡住（这是本次构建唯一一次“卡死”的原因）。

---

## 1. 产物（APK）

已构建好的 APK 在 `android/apk/`：

```
android/apk/SmartHomeBLE-debug.apk      ← 可直接安装
android/apk/SmartHomeBLE-release.apk    ← 也可直接安装（用 debug 密钥签名，仅供现场测试）
```

安装：

```bash
adb install -r "android/apk/SmartHomeBLE-debug.apk"
```

如果手机同时存在 debug 与 release 两个包：它们包名不同（`.debug` 后缀），**可以共存**，
但每次只能连一块板子（BLE 连接互斥）。

---

## 2. 用 Android Studio 打开

1. Android Studio → **Open**，选择 `android/` 目录（**不是**仓库根目录，也**不是** `app/`）。
2. 首次同步会下载 Gradle 8.9 与依赖，需要联网。Gradle JDK 选 **21**（或 Android Studio 自带 JBR）：
   `Settings → Build, Execution, Deployment → Build Tools → Gradle → Gradle JDK`。
3. `local.properties` 里已写好本机 SDK 路径，若换机器 Android Studio 会提示自动修正。
4. 直接点 ▶ Run 即可装到手机上。

## 3. 命令行构建

### 3.1 推荐：用工程自带脚本（会处理中文路径）

本工程位于含中文的目录 `C:\Users\Administrator\Desktop\esp32智能家居_客户`。
ESP32 那边用 `tools/run_idf.ps1` 把源码镜像到纯 ASCII 目录再编译，Android 这边同理，
`android/build.ps1` 会把源码 robocopy 到 `C:\smarthome_app` 构建，再把 APK 拷回 `android/apk/`。
源码仍然保留在工程目录里，你正常编辑即可。

本机**没有 PowerShell 7（pwsh）**，只有 Windows PowerShell 5.1，所以用 `powershell`：

```powershell
# Debug APK
powershell -ExecutionPolicy Bypass -File android\build.ps1

# Release APK
powershell -ExecutionPolicy Bypass -File android\build.ps1 -Task assembleRelease

# 两个都出（每次一个任务，见下方说明）
powershell -ExecutionPolicy Bypass -File android\build.ps1
powershell -ExecutionPolicy Bypass -File android\build.ps1 -Task assembleRelease

# 只用本地缓存（不联网）
powershell -ExecutionPolicy Bypass -File android\build.ps1 -Offline

# 路径已经是纯 ASCII 时想就地构建
powershell -ExecutionPolicy Bypass -File android\build.ps1 -InPlace

# 想保留常驻 Gradle daemon（只在你自己的终端里用，见下方说明）
powershell -ExecutionPolicy Bypass -File android\build.ps1 -Daemon
```

> **为什么一次只传一个任务**：PowerShell 的 `-File` 传参方式会把 `-Task a,b`
> 当成**一个**字符串 `"a,b"`，Gradle 会报 `Task 'a,b' not found`。跑两次即可。
>
> **为什么默认 `--no-daemon`**：常驻的 Gradle daemon 会继承调用者的 stdout 句柄且
> 一直不释放，任何**捕获本脚本输出**的工具（CI、agent、`Tee-Object`、管道）都会
> 永远等下去 —— 构建其实早就成功了。只在你自己终端里交互使用时才加 `-Daemon`。

### 3.2 手动 gradle（在 android/ 目录内）

```bat
cd android
gradlew.bat assembleDebug
gradlew.bat assembleRelease
```

若 `gradlew.bat` 不可用（wrapper jar 缺失），用已缓存的 Gradle 发行版：

```powershell
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"
$env:ANDROID_HOME="C:\Users\Administrator\AppData\Local\Android\Sdk"
C:\Users\Administrator\.gradle\wrapper\dists\gradle-8.9-bin\90cnw93cvbtalezasaz0blq0a\gradle-8.9\bin\gradle.bat assembleDebug
```

> ⚠️ 若在含中文的路径里直接构建遇到 `aapt2` / R8 的路径报错，就改用 3.1 的脚本（镜像构建）。

---

## 4. 首次连接手机操作步骤

1. **打开蓝牙**：系统设置里打开蓝牙（App 里点“打开蓝牙”也会跳系统授权框）。
2. **给板子上电**，确认板子正在广播（串口日志里能看到 `SmartHome-xxxxxx`）。
3. 打开 App，首次启动会弹出权限请求 → 选 **允许**：
   - Android 12+：**附近的设备**（`BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`）
   - Android 11 及以下：**位置信息**
   - 如果误点了拒绝：界面会显示红色提示，点 **重试授权**；若已被“不再询问”，
     点 **打开应用设置** 去手动开权限。
4. 点 **扫描 SmartHome-\***。列表里出现 `SmartHome-<uid>`（例如 `SmartHome-4d4a64`），
   点那一行的 **连接**。
5. 连接过程会自动：请求 **MTU 517** → `discoverServices()` → 找到 Service/RX/TX →
   给 TX 写 CCCD 开 **Notify** → 自动发一条 `0x03`(get) 让板子回一次 state + sensor。
6. 顶部连接栏会显示 **设备名 / 状态 / MTU / RSSI**。看到 `已连接，Notify 已开启，MTU 517`
   就说明链路正常，下面设备卡片和传感器面板开始有数据。
7. 如果显示 **MTU 未协商成功** 的黄色警告：单包只有 20 字节，state JSON（约 250 B）会被截断，
   传感器/设备状态可能显示不全 —— 断开重连一次，或检查板子端 MTU 协商实现。

---

## 5. 代码结构

```
android/app/src/main/java/com/smarthome/ble/
├── MainActivity.kt            单界面 Compose UI：连接栏/扫描列表/8 路设备/传感器/自动模式/日志
├── SmartHomeViewModel.kt      唯一状态源：持有 BleManager，把帧转成 Compose 状态
├── ble/BleManager.kt          全部 BLE 逻辑（扫描→连接→MTU→发现服务→Notify→get→读写队列）
├── data/Contract.kt           ★ 协议契约：UUID、类型码、设备 id、JSON 构造（与固件一一对应）
├── data/Models.kt             帧解码 + state/sensor/ack/config 数据类（缺字段全部容错）
├── data/Json.kt               手写的极简容错 JSON 解析器（省掉一个三方依赖）
└── ui/theme/Theme.kt          Material3 配色
```

### 5.1 协议实现要点（严格照契约）

* 广播名 `SmartHome-<uid>`，扫描时按**前缀** `SmartHome-` 过滤，不用固定全名。
* Service `a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d`
  RX（写）`...e`，TX（Notify）`...f`，CCCD `00002902-0000-1000-8000-00805f9b34fb`。
* 帧格式：`byte[0]` = 类型码，`byte[1..]` = UTF-8 JSON（两个方向一致）。
  下行 `0x01` cmd / `0x02` config / `0x03` get；上行 `0x01` state / `0x02` sensor /
  `0x03` ack / `0x04` event / `0x05` config。
* 连接后**必须** `requestMtu(517)`；`onMtuChanged` 成功才继续。
  若 4 秒内没回调会**自动重试一次 247**，再不行就带着警告继续工作（界面会明确标出）。
* MTU < 256 或未协商成功 → 顶部黄色警告条。
* 写操作全部串行排队（`ArrayDeque` + in-flight 标志），并且写入前按 `MTU-3` 校验长度。

### 5.2 扫描过滤为什么这样写

固件把 **128 位 Service UUID 放在主广播包**，把**设备名放在扫描响应包**
（传统广播 31 字节装不下 `Flags + 名字 + UUID`）。

所以本 App：

1. `ScanFilter.setServiceUuid(SERVICE_UUID)` 在系统层过滤 —— UUID 在主包里，最可靠、最省电；
2. `handleScan()` 里命中条件为 **“名字前缀匹配 **或** Service UUID 匹配”** ——
   这样即使扫描响应还没到、`deviceName` 还是 `null`，也不会漏掉板子；
3. 记住每个地址**曾经出现过的名字**（`nameCache`），后续回调丢名字时不会退回 `SmartHome-?`；
4. `MATCH_MODE_AGGRESSIVE` + `CALLBACK_TYPE_ALL_MATCHES` + 不批量上报，确保扫描响应一到就能拿到名字。

---

## 6. 已知限制 / 诚实说明

* **无法在无板子的情况下端到端验证 BLE 行为**。本机没有 ESP32-S3 实物，能验证到的是：
  编译通过、APK 生成、安装包结构正确、以及代码与契约逐条对照。
  **真机连板的实际收发、MTU 协商结果、Notify 是否收到数据，需要你拿手机+板子跑一遍。**
* **颜色命令在硬件上无效**：板子当前接的是普通单色 LED。界面上保留了 RGB 对话框
  和三色滑块，并明确标注“当前硬件为单色灯，颜色不生效”，为将来换 RGB 灯带预留。
* 设备卡片里的滑块采用**松手才下发**（`onValueChangeFinished`），拖动过程中不会狂刷 BLE 命令；
  同时卡片会同时显示“滑块值”和“板子回报值”，两者不一致说明命令没生效或还没回 state。
* `all/on` 的乐观 UI 是**本地推测**（4 路灯 + 风扇），真实状态以板子回的下一个 `state` 帧为准。
* 未实现固件 OTA、设备绑定/记忆、后台常驻连接（App 退到后台不保证保持连接）。
* 阈值对话框里同时也暴露了 `light_off_lux`、`temp_fan_off_c`、`fan_auto_speed`、
  `auto_window_reopen`、`enabled`（任务只要求至少能改 3 个，这里全给了）。
* Release APK 用 **debug 密钥**签名，方便直接装机测试，**不能上架应用商店**。

---

## 7. 编码注意事项（给后续维护者）

Kotlin 源码里含大量中文界面文案，**必须保持 UTF-8 无 BOM**。

⚠️ 本机只有 **Windows PowerShell 5.1**，它的 `Set-Content` / `Out-File` / `>` 默认按系统
ANSI 代码页（本机 936/GBK）写文件，**会把中文写成乱码，而且有损、不可逆**
（本次开发中已经踩过一次，`MainActivity.kt` 与 `BleManager.kt` 因此被整体重写）。

要改这两个文件（或任何含中文的文件）时：

* 用编辑器 / 文件 API，例如
  `[System.IO.File]::WriteAllText($p,$s,(New-Object System.Text.UTF8Encoding($false)))`；
* **不要**用 `Set-Content`、`Out-File`、`>` 重定向；
* 改完自检（只看会影响编译的 Kotlin 源文件）：

  ```powershell
  Get-ChildItem android -Recurse -Filter *.kt | ForEach-Object {
    $b = [System.IO.File]::ReadAllBytes($_.FullName)
    $bom = ($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
    $t = [System.Text.Encoding]::UTF8.GetString($b)
    $m = ([regex]::Matches($t, '[锛鈥鍓鎻寮濮鐨涓鏄浣鐩鍚鏂鎺璁杩鎵鍔鐢鏃鐜瀹姝鍙闇鍒鏉鍏鐐鎬]')).Count
    if ($bom -or $m) { "BAD $($_.Name) bom=$bom mojibake=$m" }
  }
  ```

  没有任何输出 = 全部干净（应无 BOM、无乱码字）。

> 说明：上面那个正则里的乱码字是本文件**故意写出来做例子**的，所以对 `README.md`
> 自己做扫描会命中；只需扫描 `*.kt` 即可。

---

## 8. 本机环境（已核实）

* JDK 21.0.9（Oracle）/ Android Studio JBR 21.0.10
* Android SDK：`C:\Users\Administrator\AppData\Local\Android\Sdk`（`ANDROID_HOME` 已设置）
* build-tools 34.0.0 / 35.0.0 / 36.1.0 / 37.0.0；platforms android-34 / 35 / 36.1
* Gradle 用户缓存里已有 8.13 / 8.9 / 8.5（本工程用 **8.9**）
* 无 PowerShell 7，只有 Windows PowerShell 5.1
