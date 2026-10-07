package com.smarthome.ble.data

// 一路设备的开关和值
data class DevState(
    val power: Boolean = false,
    val level: Int = 0,
    val r: Int = 255,
    val g: Int = 255,
    val b: Int = 255,
) {
    companion object {
        // 从文字读出设备
        fun from(o: Map<String, Any?>?): DevState = DevState(
            power = Json.boolOf(o, "power", false),
            level = Json.int(o, "level", 0).coerceIn(0, 100),
            r = Json.int(o, "r", 255).coerceIn(0, 255),
            g = Json.int(o, "g", 255).coerceIn(0, 255),
            b = Json.int(o, "b", 255).coerceIn(0, 255),
        )
    }
}

// 板子报的全部状态
data class StatePayload(
    val devices: Map<String, DevState> = emptyMap(),
    val auto: Boolean? = null,
    val rssi: Int? = null,
    val ip: String? = null,
    val uptime: Long? = null,
) {
    fun dev(id: String): DevState = devices[id] ?: DevState()

    companion object {
        // 四路灯加风扇
        private val KNOWN_DEVICE_KEYS = Proto.LAMPS + Proto.ACTUATORS + listOf(Proto.DEV_FAN)

        // 把文字变成状态
        fun from(o: Map<String, Any?>): StatePayload {
            val devs = LinkedHashMap<String, DevState>()
            for (key in KNOWN_DEVICE_KEYS) {
                val sub = Json.obj(o, key) ?: continue
                devs[key] = DevState.from(sub)
            }
            return StatePayload(
                devices = devs,
                auto = Json.bool(o, "auto"),
                rssi = Json.num(o, "rssi")?.let { Math.round(it).toInt() },
                ip = Json.str(o, "ip"),
                uptime = Json.num(o, "uptime")?.let { Math.round(it).toLong() },
            )
        }
    }
}

// 温湿度光照雨滴
data class SensorPayload(
    val temp: Double? = null,
    val humi: Double? = null,
    val tempValid: Boolean = false,
    val lux: Double? = null,
    val lightPct: Double? = null,
    val lightIsBh1750: Boolean? = null,
    val rain: Double? = null,
    val rainDetected: Boolean? = null,
) {
    companion object {
        // 从文字读出读数
        fun from(o: Map<String, Any?>): SensorPayload = SensorPayload(
            temp = Json.num(o, "temp"),
            humi = Json.num(o, "humi"),
            // 没说就当有效
            tempValid = Json.bool(o, "temp_valid") ?: (Json.num(o, "temp") != null),
            lux = Json.num(o, "lux"),
            lightPct = Json.num(o, "light_pct"),
            lightIsBh1750 = Json.bool(o, "light_is_bh1750"),
            rain = Json.num(o, "rain"),
            rainDetected = Json.bool(o, "rain_detected"),
        )
    }
}

// 一条回执或事件
data class AckPayload(val action: String?, val ok: Boolean?, val detail: String?)

// 板子回报的阈值
data class ConfigPayloadView(
    val raw: Map<String, Any?>,
    val cfg: ConfigPayload,
) {
    companion object {
        // 从文字读出阈值
        fun from(o: Map<String, Any?>): ConfigPayloadView {
            val d = ConfigPayload()
            return ConfigPayloadView(
                raw = o,
                cfg = ConfigPayload(
                    lightOnLux = Json.num(o, "light_on_lux") ?: d.lightOnLux,
                    lightOffLux = Json.num(o, "light_off_lux") ?: d.lightOffLux,
                    tempFanOnC = Json.num(o, "temp_fan_on_c") ?: d.tempFanOnC,
                    tempFanOffC = Json.num(o, "temp_fan_off_c") ?: d.tempFanOffC,
                    rainPct = Json.num(o, "rain_pct") ?: d.rainPct,
                    fanAutoSpeed = Json.num(o, "fan_auto_speed")?.let { Math.round(it).toInt() }
                        ?: d.fanAutoSpeed,
                    autoWindowReopen = Json.bool(o, "auto_window_reopen") ?: d.autoWindowReopen,
                    enabled = Json.bool(o, "enabled") ?: d.enabled,
                ),
            )
        }
    }
}

// 收到的包分几路
sealed interface Inbound {
    data class State(val payload: StatePayload, val json: String) : Inbound
    data class Sensor(val payload: SensorPayload, val json: String) : Inbound
    data class Ack(val payload: AckPayload, val json: String) : Inbound
    data class Event(val name: String?, val json: String) : Inbound
    data class Config(val payload: ConfigPayloadView, val json: String) : Inbound
    data class Unknown(val typeCode: Int, val json: String) : Inbound
}

// 按首字节认包
object FrameCodec {

    // 认包号分给各家
    fun decode(frame: ByteArray): Inbound {
        if (frame.isEmpty()) return Inbound.Unknown(-1, "")
        val type = frame[0].toInt() and 0xFF
        val json = Proto.payloadText(frame)
        val obj = Json.parseObject(json)
        if (obj == null) return Inbound.Unknown(type, json)
        return when (type) {
            Proto.Up.STATE.toInt() -> Inbound.State(StatePayload.from(obj), json)
            Proto.Up.SENSOR.toInt() -> Inbound.Sensor(SensorPayload.from(obj), json)
            Proto.Up.ACK.toInt() -> Inbound.Ack(
                AckPayload(
                    action = Json.str(obj, "action"),
                    ok = Json.bool(obj, "ok"),
                    detail = Json.str(obj, "detail"),
                ),
                json,
            )
            Proto.Up.EVENT.toInt() -> Inbound.Event(Json.str(obj, "event"), json)
            Proto.Up.CONFIG.toInt() -> Inbound.Config(ConfigPayloadView.from(obj), json)
            else -> Inbound.Unknown(type, json)
        }
    }
}
