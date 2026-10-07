package com.smarthome.ble

import android.app.Application
import android.bluetooth.BluetoothDevice
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import com.smarthome.ble.ble.BleConnectionState
import com.smarthome.ble.ble.BleManager
import com.smarthome.ble.ble.BlePhase
import com.smarthome.ble.ble.DiscoveredDevice
import com.smarthome.ble.data.ConfigPayload
import com.smarthome.ble.data.DevState
import com.smarthome.ble.data.FrameCodec
import com.smarthome.ble.data.Inbound
import com.smarthome.ble.data.Proto
import com.smarthome.ble.data.SensorPayload
import com.smarthome.ble.data.StatePayload
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// 日志里的一行
data class LogEntry(val id: Long, val time: String, val text: String, val outbound: Boolean)

class SmartHomeViewModel(app: Application) : AndroidViewModel(app), BleManager.Listener {

    private val ble = BleManager(app.applicationContext)

    // 连接情况
    var connection by mutableStateOf(BleConnectionState())
        private set
    var scanResults by mutableStateOf<List<DiscoveredDevice>>(emptyList())
        private set

    // 设备当前状态
    var state by mutableStateOf(StatePayload())
        private set
    var sensor by mutableStateOf(SensorPayload())
        private set
    var autoMode by mutableStateOf<Boolean?>(null)
        private set

    // 自动模式阈值
    var config by mutableStateOf(ConfigPayload())
        private set
    var configLoadedFromBoard by mutableStateOf(false)
        private set

    // 日志列表
    var log by mutableStateOf<List<LogEntry>>(emptyList())
        private set

    var lastAck by mutableStateOf<String?>(null)
        private set

    // 没发出去的草稿
    var thresholdDraft by mutableStateOf<ConfigPayload?>(null)
        private set

    private var logSeq = 0L
    private val timeFmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)
    private val pendingOptimistic = HashMap<String, DevState>()

    init {
        ble.listener = this
        ble.refreshAdapterState()
    }

    // 回到前台再查一次
    fun onResume() = ble.refreshAdapterState()

    override fun onCleared() {
        ble.listener = null
        ble.release()
        super.onCleared()
    }

    fun startScan() = ble.startScan()
    fun stopScan() = ble.stopScan()
    fun disconnect() = ble.disconnect()

    fun connectTo(entry: DiscoveredDevice) {
        ble.stopScan()
        ble.connect(entry.device, entry.name)
    }

    fun connectTo(device: BluetoothDevice, name: String?) = ble.connect(device, name)

    fun sendCmd(dev: String, action: String) {
        ble.send(Proto.Down.CMD, Proto.cmd(dev, action))
        optimisticallyApply(dev, action, null)
    }

    fun setLevel(dev: String, value: Int) {
        ble.send(Proto.Down.CMD, Proto.cmdSet(dev, value))
        // 等板子回报再改
    }

    fun setColor(dev: String, r: Int, g: Int, b: Int) {
        ble.send(Proto.Down.CMD, Proto.cmdColor(dev, r, g, b))
    }

    fun setAuto(on: Boolean) {
        autoMode = on
        ble.send(Proto.Down.CONFIG, Proto.cmdAuto(on))
    }

    fun requestRefresh() = ble.send(Proto.Down.GET, Proto.cmdGet())

    fun sendConfig(cfg: ConfigPayload) {
        config = cfg
        ble.send(Proto.Down.CONFIG, cfg.toJson())
    }

    fun openThresholdEditor() {
        thresholdDraft = config
    }

    fun closeThresholdEditor() {
        thresholdDraft = null
    }

    fun applyThresholdDraft() {
        thresholdDraft?.let { sendConfig(it) }
        thresholdDraft = null
    }

    fun updateDraft(transform: (ConfigPayload) -> ConfigPayload) {
        thresholdDraft = thresholdDraft?.let(transform)
    }

    fun clearLog() {
        log = emptyList()
    }

    // 先改界面后等回报
    private fun optimisticallyApply(dev: String, action: String, value: Int?) {
        fun flipped(d: DevState, on: Boolean) = d.copy(
            power = on,
            level = when {
                value != null -> value
                on && d.level == 0 -> 100
                else -> d.level
            },
        )
        val targets = if (dev == Proto.DEV_ALL) {
            // 全关含门窗帘
            if (action == "on") Proto.LAMPS + Proto.DEV_FAN else Proto.LAMPS + Proto.ACTUATORS + Proto.DEV_FAN
        } else {
            listOf(dev)
        }
        val on = when (action) {
            "on", "open" -> true
            "off", "close" -> false
            else -> return
        }
        var map = state.devices
        for (t in targets) {
            val cur = map[t] ?: DevState()
            map = map + (t to flipped(cur, on))
        }
        state = state.copy(devices = map)
    }

    // 接蓝牙那边的回调
    override fun onScanResults(devices: List<DiscoveredDevice>) {
        scanResults = devices
    }

    override fun onConnectionState(state: BleConnectionState) {
        val prev = connection
        connection = state
        if (state.rssi != null && prev.rssi != state.rssi) {
            // 信号强度已带上了
        }
        when {
            prev.phase != BlePhase.CONNECTED && state.phase == BlePhase.CONNECTED ->
                addLog("✓ 已连接 ${state.deviceName ?: state.deviceAddress}，MTU=${state.mtu ?: "?"}", false)
            prev.phase == BlePhase.CONNECTED && state.phase != BlePhase.CONNECTED ->
                addLog("连接状态变为 ${state.phase}", false)
        }
    }

    // 按类型码分派处理
    override fun onFrame(frame: ByteArray) {
        val decoded = FrameCodec.decode(frame)
        when (decoded) {
            is Inbound.State -> {
                state = decoded.payload
                decoded.payload.rssi?.let { connection = connection.copy(rssi = it) }
                decoded.payload.auto?.let { autoMode = it }
                addLog("← STATE ${decoded.json}", false)
            }
            is Inbound.Sensor -> {
                sensor = decoded.payload
                addLog("← SENSOR ${decoded.json}", false)
            }
            is Inbound.Ack -> {
                lastAck = buildString {
                    append(decoded.payload.action ?: "?")
                    append(if (decoded.payload.ok == true) " ✓" else " ✗")
                    decoded.payload.detail?.let { append(" (").append(it).append(')') }
                }
                addLog("← ACK ${decoded.json}", false)
            }
            is Inbound.Event -> addLog("← EVENT ${decoded.json}", false)
            is Inbound.Config -> {
                config = decoded.payload.cfg
                configLoadedFromBoard = true
                decoded.payload.cfg.enabled.let { autoMode = it }
                addLog("← CONFIG ${decoded.json}", false)
            }
            is Inbound.Unknown -> addLog("← 未知类型码 ${decoded.typeCode}：${decoded.json}", false)
        }
    }

    override fun onLog(line: String) = addLog(line, line.startsWith("→"))

    // 把发出去的也记上
    fun noteOutbound(text: String) = addLog(text, true)

    private fun addLog(text: String, outbound: Boolean) {
        val entry = LogEntry(
            id = logSeq++,
            time = timeFmt.format(Date()),
            text = text,
            outbound = outbound,
        )
        // 只留最近两百条
        log = (log + entry).takeLast(MAX_LOG)
    }

    // 凑顶部那几行信息
    fun diagnostics(): List<Pair<String, String>> = buildList {
        add("蓝牙" to if (!connection.adapterAvailable) "无适配器" else if (connection.bluetoothEnabled) "已开启" else "已关闭")
        add("状态" to connection.statusText)
        add("设备" to (connection.deviceName ?: "--"))
        add("地址" to (connection.deviceAddress ?: "--"))
        add("MTU" to (connection.mtu?.let { "$it（载荷 ${it - 3}B）" } ?: "未协商"))
        add("RSSI" to (connection.rssi?.let { "$it dBm" } ?: "--"))
        add("板子 IP" to (state.ip ?: "--"))
        add("uptime" to (state.uptime?.let { "${it}s" } ?: "--"))
        add("Notify" to if (ble.isNotifyEnabled()) "已开启" else "未开启")
    }

    companion object {
        private const val MAX_LOG = 200
    }
}

// 没有就用默认值
fun StatePayload.devOrNull(id: String): DevState = devices[id] ?: DevState()

// 数字留一位小数
fun fmt(v: Double?, digits: Int = 1): String =
    if (v == null || v.isNaN() || v.isInfinite()) {
        "--"
    } else {
        String.format(Locale.US, "%.${digits}f", v)
    }

// 温度无效就画横线
fun fmtTemp(p: SensorPayload, digits: Int = 1): String =
    if (!p.tempValid || p.temp == null) "--" else String.format(Locale.US, "%.${digits}f", p.temp)
