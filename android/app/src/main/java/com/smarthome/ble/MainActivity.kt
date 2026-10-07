package com.smarthome.ble

import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.smarthome.ble.ble.BleConnectionState
import com.smarthome.ble.ble.BlePermissions
import com.smarthome.ble.ble.BlePhase
import com.smarthome.ble.ble.DiscoveredDevice
import com.smarthome.ble.data.ConfigPayload
import com.smarthome.ble.data.DevState
import com.smarthome.ble.data.Proto
import com.smarthome.ble.data.SensorPayload
import com.smarthome.ble.ui.theme.SmartHomeTheme
import kotlin.math.roundToInt

class MainActivity : ComponentActivity() {

    private val vm: SmartHomeViewModel by viewModels()

    // 收下授权结果
    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        permDenied = result.filterValues { !it }.keys.toList()
        refreshPermState()
    }

    // 开完蓝牙再查一遍
    private val enableBtLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        refreshPermState()
        vm.onResume()
    }

    private var permDenied: List<String> = emptyList()
    private var permGranted = mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        refreshPermState()
        setContent {
            SmartHomeTheme {
                val granted by permGranted
                if (!granted) {
                    PermissionGate(
                        denied = permDenied,
                        onRequest = { requestPermissions() },
                    )
                } else {
                    RootScreen(vm = vm, onEnableBluetooth = { enableBluetooth() })
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshPermState()
        vm.onResume()
    }

    // 看看缺哪个权限
    private fun refreshPermState() {
        val missing = BlePermissions.missing(this)
        permGranted.value = missing.isEmpty()
        if (missing.isEmpty()) permDenied = emptyList()
    }

    // 弹出系统授权框
    private fun requestPermissions() {
        permLauncher.launch(BlePermissions.required())
    }

    // 请用户打开蓝牙
    private fun enableBluetooth() {
        try {
            enableBtLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        } catch (_: Throwable) {
            // 有的手机弹不出来
        }
        vm.onResume()
    }
}

// 没权限时挡住界面
@Composable
private fun PermissionGate(denied: List<String>, onRequest: () -> Unit) {
    val ctx = LocalContext.current
    val needsLocation = Build.VERSION.SDK_INT < Build.VERSION_CODES.S
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text("需要蓝牙权限", style = MaterialTheme.typography.headlineSmall)
        Spacer(Modifier.height(12.dp))
        Text(
            if (needsLocation) {
                "Android 11 及以下扫描 BLE 需要「位置信息」权限。"
            } else {
                "Android 12 及以上需要「附近的设备」权限才能扫描并连接 SmartHome 板子。"
            },
            style = MaterialTheme.typography.bodyMedium,
        )
        if (denied.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            Card(
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer,
                ),
            ) {
                Column(Modifier.padding(12.dp)) {
                    Text(
                        "权限被拒绝，App 无法扫描任何设备。",
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "被拒绝的项目：${denied.joinToString { it.substringAfterLast('.') }}",
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        fontSize = 12.sp,
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(
                        "可点下面的「重试」再次弹出系统授权框；" +
                            "若已勾选“不再询问”，请到 系统设置 → 应用 → 智能家居 BLE → 权限 里手动开启。",
                        color = MaterialTheme.colorScheme.onErrorContainer,
                        fontSize = 12.sp,
                    )
                }
            }
        }
        Spacer(Modifier.height(20.dp))
        Button(onClick = onRequest, modifier = Modifier.fillMaxWidth()) {
            Text(if (denied.isEmpty()) "授予蓝牙权限" else "重试授权")
        }
        Spacer(Modifier.height(8.dp))
        OutlinedButton(
            onClick = { openAppSettings(ctx) },
            modifier = Modifier.fillMaxWidth(),
        ) { Text("打开应用设置") }
    }
}

// 跳到系统设置页
private fun openAppSettings(ctx: Context) {
    try {
        ctx.startActivity(
            Intent(
                android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                android.net.Uri.fromParts("package", ctx.packageName, null),
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    } catch (_: Throwable) {
    }
}

// 主界面从上往下排
@Composable
private fun RootScreen(vm: SmartHomeViewModel, onEnableBluetooth: () -> Unit) {
    val connection = vm.connection
    val scanResults = vm.scanResults
    val devices = vm.state.devices
    val sensor = vm.sensor
    val autoMode = vm.autoMode
    val log = vm.log

    var colorTarget by remember { mutableStateOf<String?>(null) }
    val draft = vm.thresholdDraft

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
        contentPadding = PaddingValues(12.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item { HeaderCard(connection, vm, onEnableBluetooth) }

        if (!connection.adapterAvailable || !connection.bluetoothEnabled) {
            item { BluetoothOffCard(onEnableBluetooth) }
        }

        val warn = connection.warning ?: connection.lastError
        if (warn != null) {
            item { WarningCard(warn, connection.lastError != null) }
        }

        item {
            ScanAndConnectCard(
                connection = connection,
                results = scanResults,
                onScanToggle = {
                    if (connection.phase == BlePhase.SCANNING) vm.stopScan() else vm.startScan()
                },
                onDisconnect = { vm.disconnect() },
                onPick = { vm.connectTo(it) },
            )
        }

        item { AllControlsCard(vm) }

        item { SectionTitle("设备控制（8 路）") }

        items(Proto.LAMPS) { id ->
            LampCard(
                id = id,
                st = devices[id] ?: DevState(),
                onToggle = { on -> vm.sendCmd(id, if (on) "on" else "off") },
                onLevelCommit = { v -> vm.setLevel(id, v) },
                onColorClick = { colorTarget = id },
            )
        }

        item {
            FanCard(
                st = devices[Proto.DEV_FAN] ?: DevState(),
                onToggle = { on -> vm.sendCmd(Proto.DEV_FAN, if (on) "on" else "off") },
                onSpeedCommit = { v -> vm.setLevel(Proto.DEV_FAN, v) },
            )
        }

        items(Proto.ACTUATORS) { id ->
            ActuatorCard(
                id = id,
                st = devices[id] ?: DevState(),
                onOpen = { vm.sendCmd(id, "open") },
                onClose = { vm.sendCmd(id, "close") },
                onSliderCommit = { v -> vm.setLevel(id, v) },
            )
        }

        item { SectionTitle("传感器") }
        item { SensorCard(sensor) }

        item { SectionTitle("自动模式与阈值") }
        item {
            AutoCard(
                auto = autoMode,
                autoSupported = connection.phase == BlePhase.CONNECTED,
                onAuto = { vm.setAuto(it) },
                onEditThresholds = { vm.openThresholdEditor() },
                configLoaded = vm.configLoadedFromBoard,
            )
        }

        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Button(
                    onClick = { vm.requestRefresh() },
                    modifier = Modifier.weight(1f),
                    enabled = connection.phase == BlePhase.CONNECTED,
                ) { Text("刷新状态 (get)") }
                OutlinedButton(
                    onClick = { vm.clearLog() },
                    modifier = Modifier.weight(1f),
                ) { Text("清空日志") }
            }
        }

        item { SectionTitle("日志（最近 ${log.size} 条）") }
        item { LogCard(log) }

        item { Spacer(Modifier.height(24.dp)) }
    }

    colorTarget?.let { dev ->
        ColorDialog(
            id = dev,
            current = devices[dev] ?: DevState(),
            onDismiss = { colorTarget = null },
            onSend = { r, g, b ->
                vm.setColor(dev, r, g, b)
                colorTarget = null
            },
        )
    }

    draft?.let { d ->
        ThresholdDialog(
            draft = d,
            onChange = { vm.updateDraft(it) },
            onDismiss = { vm.closeThresholdEditor() },
            onApply = { vm.applyThresholdDraft() },
        )
    }
}

// 一行小标题
@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
    )
}

// 顶部状态条
@Composable
private fun HeaderCard(
    c: BleConnectionState,
    vm: SmartHomeViewModel,
    onEnableBluetooth: () -> Unit,
) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(12.dp)
                        .clip(RoundedCornerShape(6.dp))
                        .background(
                            when (c.phase) {
                                BlePhase.CONNECTED -> Color(0xFF2E7D32)
                                BlePhase.SCANNING, BlePhase.CONNECTING -> Color(0xFFF9A825)
                                BlePhase.ERROR -> Color(0xFFC62828)
                                BlePhase.IDLE -> Color(0xFF9E9E9E)
                            }
                        )
                )
                Spacer(Modifier.width(8.dp))
                Text("智能家居 BLE 控制台", style = MaterialTheme.typography.titleLarge)
            }
            Spacer(Modifier.height(10.dp))
            for ((k, v) in vm.diagnostics()) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(vertical = 1.dp)
                ) {
                    Text(
                        k,
                        fontSize = 12.sp,
                        modifier = Modifier.width(72.dp),
                        color = MaterialTheme.colorScheme.outline,
                    )
                    Text(v, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                }
            }
            if (!c.bluetoothEnabled && c.adapterAvailable) {
                Spacer(Modifier.height(8.dp))
                Button(onClick = onEnableBluetooth, modifier = Modifier.fillMaxWidth()) {
                    Text("打开蓝牙")
                }
            }
        }
    }
}

// 提醒蓝牙没开
@Composable
private fun BluetoothOffCard(onEnableBluetooth: () -> Unit) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
    ) {
        Column(Modifier.padding(12.dp)) {
            Text(
                "蓝牙未开启或不可用",
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                "请先打开系统蓝牙，然后点下面的按钮重新检查，再开始扫描。",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onErrorContainer,
            )
            Spacer(Modifier.height(8.dp))
            Button(onClick = onEnableBluetooth) { Text("打开蓝牙 / 重新检查") }
        }
    }
}

// 显示警告或错误
@Composable
private fun WarningCard(text: String, isError: Boolean) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (isError) {
                MaterialTheme.colorScheme.errorContainer
            } else {
                MaterialTheme.colorScheme.tertiaryContainer
            },
        ),
    ) {
        Text(
            (if (isError) "[错误] " else "[警告] ") + text,
            modifier = Modifier.padding(12.dp),
            fontSize = 13.sp,
            color = if (isError) {
                MaterialTheme.colorScheme.onErrorContainer
            } else {
                MaterialTheme.colorScheme.onTertiaryContainer
            },
        )
    }
}

// 扫描并点选设备
@Composable
private fun ScanAndConnectCard(
    connection: BleConnectionState,
    results: List<DiscoveredDevice>,
    onScanToggle: () -> Unit,
    onDisconnect: () -> Unit,
    onPick: (DiscoveredDevice) -> Unit,
) {
    val scanning = connection.phase == BlePhase.SCANNING
    val connected = connection.phase == BlePhase.CONNECTED ||
        connection.phase == BlePhase.CONNECTING
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("扫描 / 连接", fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onScanToggle,
                    modifier = Modifier.weight(1f),
                    enabled = connection.adapterAvailable && connection.bluetoothEnabled,
                ) { Text(if (scanning) "停止扫描" else "扫描 SmartHome-*") }
                OutlinedButton(
                    onClick = onDisconnect,
                    modifier = Modifier.weight(1f),
                    enabled = connected || scanning,
                ) { Text("断开") }
            }
            Spacer(Modifier.height(8.dp))
            Text("状态：${connection.statusText}", fontSize = 13.sp)
            Spacer(Modifier.height(6.dp))
            Text("扫到的设备（点击即连接）：", fontSize = 12.sp)
            if (results.isEmpty()) {
                Text(
                    if (scanning) "扫描中…请确认板子已上电并在广播" else "暂无结果，点上面的按钮开始扫描",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.outline,
                    modifier = Modifier.padding(vertical = 6.dp),
                )
            } else {
                results.forEach { d ->
                    Card(
                        Modifier
                            .fillMaxWidth()
                            .padding(vertical = 3.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant,
                        ),
                    ) {
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .padding(10.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Column(Modifier.weight(1f)) {
                                Text(d.name, fontWeight = FontWeight.Bold, fontSize = 14.sp)
                                Text(
                                    "${d.address}   RSSI ${d.rssi} dBm",
                                    fontSize = 11.sp,
                                    fontFamily = FontFamily.Monospace,
                                )
                            }
                            Button(onClick = { onPick(d) }) { Text("连接") }
                        }
                    }
                }
            }
        }
    }
}

// 一键全开全关
@Composable
private fun AllControlsCard(vm: SmartHomeViewModel) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text("全部控制", fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { vm.sendCmd(Proto.DEV_ALL, "on") },
                    modifier = Modifier.weight(1f),
                ) { Text("全开") }
                OutlinedButton(
                    onClick = { vm.sendCmd(Proto.DEV_ALL, "off") },
                    modifier = Modifier.weight(1f),
                ) { Text("全关") }
            }
            Spacer(Modifier.height(4.dp))
            Text(
                "全开 = 4 路灯 + 风扇；全关 = 所有设备（含门窗窗帘）",
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.outline,
            )
        }
    }
}

// 一路灯的开关和亮度
@Composable
private fun LampCard(
    id: String,
    st: DevState,
    onToggle: (Boolean) -> Unit,
    onLevelCommit: (Int) -> Unit,
    onColorClick: () -> Unit,
) {
    var slider by remember(id) { mutableStateOf(st.level.toFloat()) }
    var dragging by remember(id) { mutableStateOf(false) }
    if (!dragging && slider.roundToInt() != st.level) slider = st.level.toFloat()

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    Proto.label(id),
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    if (st.power) "开" else "关",
                    fontSize = 12.sp,
                    color = if (st.power) Color(0xFF2E7D32) else MaterialTheme.colorScheme.outline,
                    modifier = Modifier.padding(end = 8.dp),
                )
                Switch(checked = st.power, onCheckedChange = onToggle)
            }
            Spacer(Modifier.height(4.dp))
            Text("亮度 ${slider.roundToInt()}%（板子回报 ${st.level}%）", fontSize = 12.sp)
            Slider(
                value = slider,
                onValueChange = {
                    dragging = true
                    slider = it
                },
                onValueChangeFinished = {
                    dragging = false
                    onLevelCommit(slider.roundToInt())
                },
                valueRange = 0f..100f,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(22.dp)
                        .clip(RoundedCornerShape(4.dp))
                        .background(Color(st.r, st.g, st.b))
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    "RGB(${st.r},${st.g},${st.b})",
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.weight(1f),
                )
                OutlinedButton(onClick = onColorClick) { Text("颜色") }
            }
            Text(
                "当前硬件为单色灯，颜色不生效",
                fontSize = 10.sp,
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
}

// 风扇开关和转速
@Composable
private fun FanCard(st: DevState, onToggle: (Boolean) -> Unit, onSpeedCommit: (Int) -> Unit) {
    var slider by remember { mutableStateOf(st.level.toFloat()) }
    var dragging by remember { mutableStateOf(false) }
    if (!dragging && slider.roundToInt() != st.level) slider = st.level.toFloat()

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("风扇", fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                Text(
                    if (st.power) "开" else "关",
                    fontSize = 12.sp,
                    color = if (st.power) Color(0xFF2E7D32) else MaterialTheme.colorScheme.outline,
                    modifier = Modifier.padding(end = 8.dp),
                )
                Switch(checked = st.power, onCheckedChange = onToggle)
            }
            Spacer(Modifier.height(4.dp))
            Text("转速 ${slider.roundToInt()}%（板子回报 ${st.level}%）", fontSize = 12.sp)
            Slider(
                value = slider,
                onValueChange = {
                    dragging = true
                    slider = it
                },
                onValueChangeFinished = {
                    dragging = false
                    onSpeedCommit(slider.roundToInt())
                },
                valueRange = 0f..100f,
            )
        }
    }
}

// 窗门帘的开合度
@Composable
private fun ActuatorCard(
    id: String,
    st: DevState,
    onOpen: () -> Unit,
    onClose: () -> Unit,
    onSliderCommit: (Int) -> Unit,
) {
    var slider by remember(id) { mutableStateOf(st.level.toFloat()) }
    var dragging by remember(id) { mutableStateOf(false) }
    if (!dragging && slider.roundToInt() != st.level) slider = st.level.toFloat()

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    Proto.label(id),
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f),
                )
                Text("开合度 ${slider.roundToInt()}%", fontSize = 12.sp)
            }
            Slider(
                value = slider,
                onValueChange = {
                    dragging = true
                    slider = it
                },
                onValueChangeFinished = {
                    dragging = false
                    onSliderCommit(slider.roundToInt())
                },
                valueRange = 0f..100f,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onOpen, modifier = Modifier.weight(1f)) { Text("开") }
                OutlinedButton(onClick = onClose, modifier = Modifier.weight(1f)) { Text("关") }
            }
            Text(
                "「开」= open（板子等价于 set value=100），「关」= close（等价 set value=0）",
                fontSize = 10.sp,
                color = MaterialTheme.colorScheme.outline,
            )
        }
    }
}

// 显示五路传感器读数
@Composable
private fun SensorCard(s: SensorPayload) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            SensorRow("温度", fmtTemp(s) + if (s.tempValid) " °C" else "（temp_valid=false）")
            SensorRow("湿度", fmt(s.humi) + " %")
            SensorRow("光照", "${fmt(s.lightPct)} %   (${fmt(s.lux)} lux)")
            SensorRow(
                "光照来源",
                when (s.lightIsBh1750) {
                    true -> "BH1750 数字光照传感器"
                    false -> "光敏电阻估算"
                    null -> "--"
                },
            )
            SensorRow("雨滴", fmt(s.rain) + " %")
            SensorRow(
                "下雨检测",
                when (s.rainDetected) {
                    true -> "检测到下雨"
                    false -> "未检测到"
                    null -> "--"
                },
            )
        }
    }
}

// 一行名字加读数
@Composable
private fun SensorRow(k: String, v: String) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp)
    ) {
        Text(
            k,
            fontSize = 13.sp,
            modifier = Modifier.width(88.dp),
            color = MaterialTheme.colorScheme.outline,
        )
        Text(v, fontSize = 13.sp, fontFamily = FontFamily.Monospace)
    }
}

// 自动模式开关和入口
@Composable
private fun AutoCard(
    auto: Boolean?,
    autoSupported: Boolean,
    onAuto: (Boolean) -> Unit,
    onEditThresholds: () -> Unit,
    configLoaded: Boolean,
) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("自动模式", fontWeight = FontWeight.Bold)
                    Text(
                        when (auto) {
                            true -> "已开启（板子回报 auto=true）"
                            false -> "已关闭"
                            null -> "板子尚未回报 auto 字段"
                        },
                        fontSize = 11.sp,
                        color = MaterialTheme.colorScheme.outline,
                    )
                }
                Switch(
                    checked = auto == true,
                    onCheckedChange = onAuto,
                    enabled = autoSupported,
                )
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = onEditThresholds,
                modifier = Modifier.fillMaxWidth(),
                enabled = autoSupported,
            ) { Text(if (configLoaded) "改阈值（已从板子读取）" else "改阈值") }
            if (!autoSupported) {
                Spacer(Modifier.height(4.dp))
                Text("连接后可操作", fontSize = 11.sp, color = MaterialTheme.colorScheme.outline)
            }
        }
    }
}

// 显示收发日志
@Composable
private fun LogCard(log: List<LogEntry>) {
    Card(Modifier.fillMaxWidth()) {
        Column(
            Modifier
                .fillMaxWidth()
                .height(260.dp)
                .padding(8.dp)
        ) {
            if (log.isEmpty()) {
                Text(
                    "暂无日志。连接成功后这里会显示收发的原始 JSON。",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.outline,
                )
            } else {
                LazyColumn(Modifier.fillMaxSize()) {
                    items(log.asReversed()) { e ->
                        Text(
                            "${e.time} ${e.text}",
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            color = if (e.outbound) {
                                Color(0xFF1565C0)
                            } else {
                                MaterialTheme.colorScheme.onSurface
                            },
                            modifier = Modifier.padding(vertical = 1.dp),
                        )
                    }
                }
            }
        }
    }
}

// 挑灯的颜色
@Composable
private fun ColorDialog(
    id: String,
    current: DevState,
    onDismiss: () -> Unit,
    onSend: (Int, Int, Int) -> Unit,
) {
    var r by remember { mutableStateOf(current.r.toFloat()) }
    var g by remember { mutableStateOf(current.g.toFloat()) }
    var b by remember { mutableStateOf(current.b.toFloat()) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("${Proto.label(id)} 颜色") },
        text = {
            Column {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                        .clip(RoundedCornerShape(6.dp))
                        .background(Color(r.roundToInt(), g.roundToInt(), b.roundToInt()))
                )
                Spacer(Modifier.height(10.dp))
                RgbSlider("R", r) { r = it }
                RgbSlider("G", g) { g = it }
                RgbSlider("B", b) { b = it }
                Spacer(Modifier.height(6.dp))
                Text(
                    "当前硬件为单色 LED，颜色命令在硬件上无效，仅备将来换 RGB 灯带时使用。",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.error,
                )
                Spacer(Modifier.height(6.dp))
                Text(
                    """{"dev":"$id","action":"color",""" +
                        """"r":${r.roundToInt()},"g":${g.roundToInt()},"b":${b.roundToInt()}}""",
                    fontSize = 10.sp,
                    fontFamily = FontFamily.Monospace,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { onSend(r.roundToInt(), g.roundToInt(), b.roundToInt()) }) {
                Text("发送 color")
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("取消") } },
    )
}

// 一条颜色滑条
@Composable
private fun RgbSlider(label: String, value: Float, onChange: (Float) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, modifier = Modifier.width(20.dp), fontSize = 12.sp)
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = 0f..255f,
            modifier = Modifier.weight(1f),
        )
        Text(value.roundToInt().toString(), modifier = Modifier.width(40.dp), fontSize = 12.sp)
    }
}

// 改自动模式的阈值
@Composable
private fun ThresholdDialog(
    draft: ConfigPayload,
    onChange: ((ConfigPayload) -> ConfigPayload) -> Unit,
    onDismiss: () -> Unit,
    onApply: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("自动模式阈值") },
        text = {
            Column(
                Modifier
                    .fillMaxWidth()
                    .height(380.dp)
            ) {
                LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    item {
                        NumField("temp_fan_on_c（温度 ≥ 此值开风扇）", draft.tempFanOnC) { v ->
                            onChange { it.copy(tempFanOnC = v) }
                        }
                    }
                    item {
                        NumField("light_on_lux（光照 < 此值开灯）", draft.lightOnLux) { v ->
                            onChange { it.copy(lightOnLux = v) }
                        }
                    }
                    item {
                        NumField("rain_pct（雨滴 ≥ 此值关窗）", draft.rainPct) { v ->
                            onChange { it.copy(rainPct = v) }
                        }
                    }
                    item {
                        NumField("light_off_lux（光照 > 此值关灯）", draft.lightOffLux) { v ->
                            onChange { it.copy(lightOffLux = v) }
                        }
                    }
                    item {
                        NumField("temp_fan_off_c（温度 ≤ 此值关风扇）", draft.tempFanOffC) { v ->
                            onChange { it.copy(tempFanOffC = v) }
                        }
                    }
                    item {
                        NumField(
                            "fan_auto_speed（自动模式转速 %）",
                            draft.fanAutoSpeed.toDouble(),
                        ) { v -> onChange { it.copy(fanAutoSpeed = v.roundToInt()) } }
                    }
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                "auto_window_reopen（下雨关窗后自动重开）",
                                modifier = Modifier.weight(1f),
                                fontSize = 13.sp,
                            )
                            Switch(
                                checked = draft.autoWindowReopen,
                                onCheckedChange = { v -> onChange { it.copy(autoWindowReopen = v) } },
                            )
                        }
                    }
                    item {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                "enabled（自动模式总开关）",
                                modifier = Modifier.weight(1f),
                                fontSize = 13.sp,
                            )
                            Switch(
                                checked = draft.enabled,
                                onCheckedChange = { v -> onChange { it.copy(enabled = v) } },
                            )
                        }
                    }
                    item {
                        Text(
                            "发送 JSON：\n" + draft.toJson(),
                            fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onApply) { Text("发送 config(0x02)") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("取消") } },
    )
}

// 一个数字输入框
@Composable
private fun NumField(label: String, value: Double, onValue: (Double) -> Unit) {
    var text by remember(label) { mutableStateOf(trimNum(value)) }
    OutlinedTextField(
        value = text,
        onValueChange = { s ->
            text = s
            s.trim().toDoubleOrNull()?.let(onValue)
        },
        label = { Text(label, fontSize = 11.sp) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
}

// 整数不显示小数点
private fun trimNum(v: Double): String =
    if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()
