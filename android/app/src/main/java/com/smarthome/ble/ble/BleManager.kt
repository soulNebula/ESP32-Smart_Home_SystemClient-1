package com.smarthome.ble.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import com.smarthome.ble.data.Proto
import java.util.ArrayDeque

enum class BlePhase { IDLE, SCANNING, CONNECTING, CONNECTED, ERROR }

data class DiscoveredDevice(
    val name: String,
    val address: String,
    val rssi: Int,
    val device: BluetoothDevice,
)

data class BleConnectionState(
    val adapterAvailable: Boolean = false,
    val bluetoothEnabled: Boolean = false,
    val phase: BlePhase = BlePhase.IDLE,
    val deviceName: String? = null,
    val deviceAddress: String? = null,
    val rssi: Int? = null,
    // 谈好的单包大小
    val mtu: Int? = null,
    val mtuRequested: Int = MTU_TARGET,
    val mtuOk: Boolean = false,
    val warning: String? = null,
    val lastError: String? = null,
    val statusText: String = "未连接",
) {
    companion object {
        const val MTU_TARGET = 517
        // 够放一包状态
        const val MTU_MIN = 256
    }
}

// 查有没有蓝牙权限
object BlePermissions {
    // 按系统版本要权限
    fun required(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    fun granted(ctx: Context): Boolean = required().all {
        ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED
    }

    fun missing(ctx: Context): List<String> = required().filter {
        ContextCompat.checkSelfPermission(ctx, it) != PackageManager.PERMISSION_GRANTED
    }
}

class BleManager(private val appContext: Context) {

    interface Listener {
        fun onScanResults(devices: List<DiscoveredDevice>)
        fun onConnectionState(state: BleConnectionState)
        fun onFrame(frame: ByteArray)
        fun onLog(line: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val thread = HandlerThread("ble-gatt").apply { start() }
    private val h = Handler(thread.looper)

    private val btManager: BluetoothManager? =
        appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? = btManager?.adapter

    @Volatile
    var listener: Listener? = null

    // 状态只在后台线程动
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var connected = false
    private var stage = Stage.IDLE
    private var notifyEnabled = false
    private val writeQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false
    private var mtuFallbackTried = false
    private var mtuToken = 0
    private val seen = LinkedHashMap<String, DiscoveredDevice>()
    // 记住板子报过的名字
    private val nameCache = HashMap<String, String>()
    private var st = BleConnectionState()
    private var scanning = false

    private enum class Stage { IDLE, CONNECTING, MTU, SERVICES, NOTIFY, READY }

    private companion object {
        const val MTU_TIMEOUT_MS = 4000L
    }

    // 查一下蓝牙开着没
    fun refreshAdapterState() {
        h.post {
            val available = adapter != null
            val enabled = try {
                adapter?.isEnabled == true
            } catch (_: SecurityException) {
                false
            }
            st = st.copy(
                adapterAvailable = available,
                bluetoothEnabled = enabled,
                phase = if (available) st.phase else BlePhase.ERROR,
                statusText = when {
                    !available -> "本机没有蓝牙适配器"
                    !enabled -> "蓝牙已关闭，请打开蓝牙"
                    st.phase == BlePhase.IDLE -> "未连接"
                    else -> st.statusText
                },
            )
            emitState()
        }
    }

    // 开始找板子
    @SuppressLint("MissingPermission")
    fun startScan() {
        h.post {
            if (!checkPerms()) { warn("缺少蓝牙权限，无法扫描"); return@post }
            val ad = adapter
            if (ad == null) { warn("本机没有蓝牙适配器"); return@post }
            if (!ad.isEnabled) { warn("蓝牙未打开，请先在系统里打开蓝牙"); return@post }
            val scanner = ad.bluetoothLeScanner
            if (scanner == null) { warn("无法获取 BLE 扫描器"); return@post }

            // 认服务号不认名字
            val filters = listOf(
                ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(Proto.SERVICE_UUID))
                    .build(),
            )
            val settings = ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                // 来一条报一条
                .setReportDelay(0L)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .setMatchMode(ScanSettings.MATCH_MODE_AGGRESSIVE)
                .setNumOfMatches(ScanSettings.MATCH_NUM_ONE_ADVERTISEMENT)
                .build()

            seen.clear()
            nameCache.clear()
            publishScanResults()
            scanning = true
            st = st.copy(phase = BlePhase.SCANNING, statusText = "扫描中…", lastError = null)
            emitState()
            log("开始扫描（按名称前缀 ${Proto.NAME_PREFIX} 过滤）")
            try {
                scanner.startScan(filters, settings, scanCallback)
            } catch (e: Throwable) {
                // 过滤不稳就自己筛
                log("带过滤的扫描启动失败(${e.javaClass.simpleName})，改为无过滤扫描")
                try {
                    scanner.startScan(null, settings, scanCallback)
                } catch (e2: Throwable) {
                    scanning = false
                    warn("扫描启动失败：${e2.message}")
                }
            }
        }
    }

    // 停止找板子
    @SuppressLint("MissingPermission")
    fun stopScan() {
        h.post {
            if (!scanning) return@post
            scanning = false
            try {
                adapter?.bluetoothLeScanner?.stopScan(scanCallback)
            } catch (_: Throwable) {
            }
            if (st.phase == BlePhase.SCANNING) {
                st = st.copy(phase = BlePhase.IDLE, statusText = "未连接")
            }
            log("停止扫描")
            emitState()
        }
    }

    // 连上一块板子
    @SuppressLint("MissingPermission")
    fun connect(dev: BluetoothDevice, name: String?) {
        h.post {
            if (!checkPerms()) { warn("缺少蓝牙权限，无法连接"); return@post }
            disconnectInternal(silent = false)

            stage = Stage.CONNECTING
            connected = false
            notifyEnabled = false
            writeQueue.clear()
            writeInFlight = false
            mtuFallbackTried = false
            mtuToken++

            val label = name ?: safeName(dev)
            st = st.copy(
                phase = BlePhase.CONNECTING,
                deviceName = label,
                deviceAddress = dev.address,
                rssi = st.rssi,
                mtu = null,
                mtuOk = false,
                warning = null,
                lastError = null,
                statusText = "正在连接 $label …",
            )
            emitState()
            log("连接 ${dev.address} ($label)")

            try {
                gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    dev.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
                } else {
                    dev.connectGatt(appContext, false, gattCallback)
                }
            } catch (e: SecurityException) {
                stage = Stage.IDLE
                fail("连接失败：缺少 BLUETOOTH_CONNECT 权限")
            } catch (e: Throwable) {
                stage = Stage.IDLE
                fail("连接异常：${e.message}")
            }
        }
    }

    // 断开这块板子
    @SuppressLint("MissingPermission")
    fun disconnect() {
        h.post { disconnectInternal(silent = false) }
    }

    // 发一包命令给板子
    fun send(type: Byte, json: String) {
        val frame = Proto.frame(type, json)
        h.post {
            if (!connected || gatt == null) {
                warn("未连接，命令未发送：$json")
                return@post
            }
            val mtu = st.mtu
            if (mtu != null && frame.size > mtu - 3) {
                warn("帧 ${frame.size}B 超过 MTU 载荷上限 ${mtu - 3}B，未发送")
                return@post
            }
            writeQueue.addLast(frame)
            pumpWrites()
        }
    }

    fun release() {
        h.post {
            disconnectInternal(silent = true)
            thread.quitSafely()
        }
    }

    // 把新状态报给界面
    private fun emitState() {
        val snapshot = st
        main.post { listener?.onConnectionState(snapshot) }
    }

    private fun log(line: String) {
        main.post { listener?.onLog(line) }
    }

    // 记一句提醒
    private fun warn(msg: String) {
        st = st.copy(warning = msg)
        emitState()
        log("WARN $msg")
    }

    // 记一句错误
    private fun fail(msg: String) {
        st = st.copy(phase = BlePhase.ERROR, lastError = msg, statusText = msg)
        emitState()
        log("ERROR $msg")
    }

    private fun checkPerms(): Boolean = BlePermissions.granted(appContext)

    @SuppressLint("MissingPermission")
    private fun safeName(dev: BluetoothDevice): String? = try {
        dev.name
    } catch (_: SecurityException) {
        null
    }

    // 把结果报给界面
    private fun publishScanResults() {
        val list = seen.values.sortedByDescending { it.rssi }
        main.post { listener?.onScanResults(list) }
    }

    @SuppressLint("MissingPermission")
    private fun disconnectInternal(silent: Boolean) {
        if (scanning) {
            scanning = false
            try {
                adapter?.bluetoothLeScanner?.stopScan(scanCallback)
            } catch (_: Throwable) {
            }
        }
        stage = Stage.IDLE
        connected = false
        notifyEnabled = false
        writeQueue.clear()
        writeInFlight = false
        mtuToken++
        val g = gatt
        gatt = null
        rxChar = null
        txChar = null
        if (g != null) {
            try {
                g.disconnect()
            } catch (_: Throwable) {
            }
            try {
                g.close()
            } catch (_: Throwable) {
            }
        }
        if (!silent) {
            st = st.copy(
                phase = BlePhase.IDLE,
                mtu = null,
                mtuOk = false,
                warning = null,
                lastError = null,
                statusText = "未连接",
            )
            emitState()
            log("已断开连接")
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handleScan(result)
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            results.forEach { handleScan(it) }
        }

        override fun onScanFailed(errorCode: Int) {
            h.post {
                scanning = false
                fail("扫描失败，错误码 $errorCode")
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun handleScan(result: ScanResult) {
        val dev = result.device ?: return
        val rec = result.scanRecord

        // 名字可能晚点到
        val recName = rec?.deviceName
        val cachedName = try {
            dev.name
        } catch (_: SecurityException) {
            null
        }
        val name = recName ?: cachedName ?: nameCache[dev.address]

        // 服务号最靠得住
        val hasService = rec?.serviceUuids?.any { it.uuid == Proto.SERVICE_UUID } == true

        // 名字或服务号对得上
        val nameMatches = name != null && name.startsWith(Proto.NAME_PREFIX)
        if (!nameMatches && !hasService) return

        if (name != null) nameCache[dev.address] = name

        val entry = DiscoveredDevice(
            name = name ?: "SmartHome-?",
            address = dev.address,
            rssi = result.rssi,
            device = dev,
        )
        h.post {
            val prev = seen[entry.address]
            if (prev == null || prev.rssi != entry.rssi || prev.name != entry.name) {
                seen[entry.address] = entry
                publishScanResults()
            }
            if (st.rssi == null) {
                st = st.copy(rssi = entry.rssi)
                emitState()
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            h.post {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        connected = true
                        stage = Stage.MTU
                        st = st.copy(
                            phase = BlePhase.CONNECTING,
                            statusText = "已连接，协商 MTU ${BleConnectionState.MTU_TARGET} …",
                        )
                        emitState()
                        log("GATT 已连接，请求 MTU=${BleConnectionState.MTU_TARGET}")
                        val ok = try {
                            g.requestMtu(BleConnectionState.MTU_TARGET)
                        } catch (e: Throwable) {
                            log("requestMtu 抛异常：${e.message}")
                            false
                        }
                        if (!ok) {
                            onMtuFailed("requestMtu() 返回 false，跳过 MTU 协商")
                        } else {
                            armMtuTimeout(BleConnectionState.MTU_TARGET)
                        }
                    }

                    BluetoothProfile.STATE_DISCONNECTED -> {
                        connected = false
                        val wasReady = stage == Stage.READY
                        stage = Stage.IDLE
                        mtuToken++
                        writeQueue.clear()
                        writeInFlight = false
                        rxChar = null
                        txChar = null
                        st = st.copy(
                            phase = BlePhase.IDLE,
                            mtu = null,
                            mtuOk = false,
                            statusText = "连接已断开" +
                                if (status != 0) "（status=$status）" else "",
                            warning = if (wasReady) null else st.warning,
                        )
                        emitState()
                        log("GATT 断开 (status=$status)")
                        try {
                            g.close()
                        } catch (_: Throwable) {
                        }
                        if (gatt === g) gatt = null
                    }

                    else -> log("连接状态变化：$newState (status=$status)")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            h.post {
                if (stage != Stage.MTU) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log("onMtuChanged 失败 status=$status")
                    onMtuFailed("MTU 协商失败（status=$status）")
                    return@post
                }
                val frame = mtu - 3
                val ok = mtu >= BleConnectionState.MTU_MIN
                st = st.copy(
                    mtu = mtu,
                    mtuOk = ok,
                    warning = if (ok) null else
                        "MTU 只有 $mtu（单包载荷 ${frame}B），state JSON 约 250B 可能被截断",
                )
                emitState()
                log("MTU 协商成功：$mtu（单包载荷 ${frame}B）")
                if (!ok) log("WARN MTU < ${BleConnectionState.MTU_MIN}，通知数据可能被截断")
                stage = Stage.SERVICES
                discoverServices(g)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            h.post {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("discoverServices 失败 status=$status")
                    return@post
                }
                val svc = g.getService(Proto.SERVICE_UUID)
                if (svc == null) {
                    fail("未找到服务 ${Proto.SERVICE_UUID}，请确认连的是 SmartHome 板子")
                    return@post
                }
                val rx = svc.getCharacteristic(Proto.RX_CHAR_UUID)
                val tx = svc.getCharacteristic(Proto.TX_CHAR_UUID)
                if (rx == null || tx == null) {
                    fail(
                        "缺少特征：RX=${rx != null} TX=${tx != null}" +
                            "（期望 ${Proto.RX_CHAR_UUID} / ${Proto.TX_CHAR_UUID}）"
                    )
                    return@post
                }
                rxChar = rx
                txChar = tx
                log(
                    "服务已发现：RX flags=0x%02X, TX flags=0x%02X".format(
                        rx.properties, tx.properties,
                    )
                )
                stage = Stage.NOTIFY
                enableNotify(g, tx)
            }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            h.post {
                if (descriptor.uuid != Proto.CCCD_UUID) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    notifyEnabled = false
                    warn("开启 Notify 失败（status=$status），仍尝试发 get")
                } else {
                    notifyEnabled = true
                    log("Notify 已开启（CCCD 写入成功）")
                }
                stage = Stage.READY
                st = st.copy(
                    phase = BlePhase.CONNECTED,
                    statusText = buildString {
                        append("已连接，Notify ")
                        append(if (notifyEnabled) "已开启" else "未确认")
                        st.mtu?.let { append("，MTU $it") }
                    },
                )
                emitState()
                // 连上先要一次状态
                log("发送 get(0x03) 请求一次 state+sensor")
                sendNow(g, Proto.frame(Proto.Down.GET, Proto.cmdGet()))
                pumpWrites()
            }
        }

        @Deprecated("Deprecated in API 33, still the only path below it")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            val data = characteristic.value ?: ByteArray(0)
            deliverFrame(data)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            deliverFrame(value)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            h.post {
                writeInFlight = false
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    log("ERROR 写入失败 status=$status")
                }
                pumpWrites()
            }
        }
    }

    // 把收到的包交上去
    private fun deliverFrame(data: ByteArray) {
        if (data.isEmpty()) return
        main.post { listener?.onFrame(data) }
    }

    @SuppressLint("MissingPermission")
    private fun discoverServices(g: BluetoothGatt) {
        val ok = try {
            g.discoverServices()
        } catch (e: Throwable) {
            log("discoverServices 抛异常：${e.message}")
            false
        }
        if (!ok) fail("discoverServices() 返回 false")
    }

    // 谈不成就带警告继续
    private fun onMtuFailed(reason: String) {
        st = st.copy(
            mtu = null,
            mtuOk = false,
            warning = "MTU 未协商成功（$reason）" +
                "默认 MTU 23 时单包只有 20 字节，state JSON（约 250B）会被截断，" +
                "传感器和设备状态可能显示不全，请重连，或在板子端确认 MTU 协商",
            statusText = "已连接（MTU 未协商）",
        )
        emitState()
        val g = gatt ?: return
        stage = Stage.SERVICES
        discoverServices(g)
    }

    // 超时换个小值再试
    private fun armMtuTimeout(requested: Int) {
        val token = ++mtuToken
        h.postDelayed({
            if (stage != Stage.MTU || token != mtuToken) return@postDelayed
            if (!mtuFallbackTried) {
                mtuFallbackTried = true
                log("WARN ${MTU_TIMEOUT_MS}ms 内没有收到 onMtuChanged，改试 MTU=247")
                val g = gatt
                val ok = try {
                    g?.requestMtu(247) ?: false
                } catch (_: Throwable) {
                    false
                }
                if (ok) armMtuTimeout(247) else onMtuFailed("requestMtu(247) 也失败")
            } else {
                onMtuFailed("等待 onMtuChanged 超时（已试 $requested 与 247）")
            }
        }, MTU_TIMEOUT_MS)
    }

    // 打开板子的推送
    @SuppressLint("MissingPermission")
    private fun enableNotify(g: BluetoothGatt, tx: BluetoothGattCharacteristic) {
        val hasNotify = (tx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY) != 0
        val hasIndicate = (tx.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0
        if (!hasNotify && !hasIndicate) {
            warn("TX 特征不支持 Notify/Indicate，无法接收板子数据")
        }
        try {
            val ok = g.setCharacteristicNotification(tx, true)
            log("setCharacteristicNotification = $ok")
            if (!ok) warn("setCharacteristicNotification 返回 false")
        } catch (e: Throwable) {
            warn("开启本地通知失败：${e.message}")
        }
        val cccd = tx.getDescriptor(Proto.CCCD_UUID)
        if (cccd == null) {
            warn("TX 特征上没有 CCCD 描述符 ${Proto.CCCD_UUID}，通知可能收不到")
            stage = Stage.READY
            st = st.copy(phase = BlePhase.CONNECTED, statusText = "已连接（无 CCCD）")
            emitState()
            sendNow(g, Proto.frame(Proto.Down.GET, Proto.cmdGet()))
            return
        }
        val value = if (hasIndicate && !hasNotify) {
            BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
        } else {
            BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        }
        cccd.value = value
        val started = try {
            g.writeDescriptor(cccd)
        } catch (e: Throwable) {
            log("writeDescriptor 抛异常：${e.message}")
            false
        }
        if (!started) {
            warn("写 CCCD 失败，Notify 未开启；仍会尝试 get")
            stage = Stage.READY
            st = st.copy(phase = BlePhase.CONNECTED, statusText = "已连接（Notify 未开启）")
            emitState()
            sendNow(g, Proto.frame(Proto.Down.GET, Proto.cmdGet()))
        }
    }

    // 插队马上发一包
    @SuppressLint("MissingPermission")
    private fun sendNow(g: BluetoothGatt, frame: ByteArray) {
        val rx = rxChar ?: run { warn("RX 特征未就绪，无法发送"); return }
        writeFrame(g, rx, frame) { result ->
            if (result) writeInFlight = true
        }
    }

    // 排队一包一包发
    @SuppressLint("MissingPermission")
    private fun pumpWrites() {
        if (writeInFlight) return
        val g = gatt ?: return
        val rx = rxChar ?: return
        val frame = writeQueue.pollFirst() ?: return
        writeFrame(g, rx, frame) { ok -> writeInFlight = ok }
    }

    // 真正写一包出去
    @SuppressLint("MissingPermission")
    private fun writeFrame(
        g: BluetoothGatt,
        rx: BluetoothGattCharacteristic,
        frame: ByteArray,
        onResult: (Boolean) -> Unit,
    ) {
        val mtu = st.mtu
        if (mtu != null && frame.size > mtu - 3) {
            warn("帧 ${frame.size}B 超过 MTU 载荷 ${mtu - 3}B，已丢弃")
            onResult(false)
            return
        }
        // 能不回执就不回执
        val noRsp =
            (rx.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0 &&
                (rx.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) == 0
        rx.writeType = if (noRsp) {
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        } else {
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }

        val text = Proto.payloadText(frame)
        val typeHex = "0x%02X".format(frame[0])
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                val code = g.writeCharacteristic(rx, frame, rx.writeType)
                val ok = code == BluetoothStatusCodes.SUCCESS
                if (ok) log("TX $typeHex $text") else log("ERROR 写入被拒 (code=$code): $text")
                onResult(ok)
            } else {
                @Suppress("DEPRECATION")
                rx.value = frame
                @Suppress("DEPRECATION")
                val ok = g.writeCharacteristic(rx)
                if (ok) log("TX $typeHex $text") else log("ERROR 写入失败: $text")
                onResult(ok)
            }
        } catch (e: SecurityException) {
            log("ERROR 写入被拒绝：缺少权限")
            onResult(false)
        } catch (e: Throwable) {
            log("ERROR 写入异常：${e.message}")
            onResult(false)
        }
    }

    // 问一下通知开了没
    fun isNotifyEnabled(): Boolean = notifyEnabled
}
