package com.smarthome.ble.data

import java.nio.charset.StandardCharsets
import java.util.UUID

object Proto {

    // 板子广播名前缀
    const val NAME_PREFIX = "SmartHome-"

    val SERVICE_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d")

    // 手机发给板子
    val RX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e")

    // 板子推给手机
    val TX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f")

    // 开关通知的那一格
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // 手机往下的包号
    object Down {
        const val CMD: Byte = 0x01
        const val CONFIG: Byte = 0x02
        const val GET: Byte = 0x03
    }

    // 板子往上的包号
    object Up {
        const val STATE: Byte = 0x01
        const val SENSOR: Byte = 0x02
        const val ACK: Byte = 0x03
        const val EVENT: Byte = 0x04
        const val CONFIG: Byte = 0x05
    }

    const val DEV_ALL = "all"
    const val DEV_LED_LIVING = "led_living"
    const val DEV_LED_KITCHEN = "led_kitchen"
    const val DEV_LED_BEDROOM = "led_bedroom"
    const val DEV_LED_BATH = "led_bath"
    const val DEV_FAN = "fan"
    const val DEV_WINDOW = "window"
    const val DEV_DOOR = "door"
    const val DEV_CURTAIN = "curtain"

    // 四路灯的先后
    val LAMPS = listOf(DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH)

    // 窗门帘的先后
    val ACTUATORS = listOf(DEV_WINDOW, DEV_DOOR, DEV_CURTAIN)

    // 把代号翻成中文
    fun label(dev: String): String = when (dev) {
        DEV_LED_LIVING -> "客厅灯"
        DEV_LED_KITCHEN -> "厨房灯"
        DEV_LED_BEDROOM -> "卧室灯"
        DEV_LED_BATH -> "浴室灯"
        DEV_FAN -> "风扇"
        DEV_WINDOW -> "窗户"
        DEV_DOOR -> "门"
        DEV_CURTAIN -> "窗帘"
        DEV_ALL -> "全部"
        else -> dev
    }

    // 首字节带包号
    fun frame(type: Byte, json: String): ByteArray {
        val body = json.toByteArray(StandardCharsets.UTF_8)
        val out = ByteArray(body.size + 1)
        out[0] = type
        System.arraycopy(body, 0, out, 1, body.size)
        return out
    }

    // 去掉包号留下文字
    fun payloadText(frame: ByteArray): String {
        if (frame.size <= 1) return ""
        return String(frame, 1, frame.size - 1, StandardCharsets.UTF_8)
    }

    // 拼开关命令
    fun cmd(dev: String, action: String): String = """{"dev":"$dev","action":"$action"}"""

    // 拼调值命令
    fun cmdSet(dev: String, value: Int): String =
        """{"dev":"$dev","action":"set","value":${value.coerceIn(0, 100)}}"""

    // 拼调色命令
    fun cmdColor(dev: String, r: Int, g: Int, b: Int): String =
        """{"dev":"$dev","action":"color","r":${r.coerceIn(0, 255)},""" +
            """"g":${g.coerceIn(0, 255)},"b":${b.coerceIn(0, 255)}}"""

    // 拼自动模式命令
    fun cmdAuto(auto: Boolean): String = """{"action":"auto","value":$auto}"""

    // 拼要一次状态
    fun cmdGet(): String = """{"get":"state"}"""
}

// 存自动模式的阈值
data class ConfigPayload(
    val lightOnLux: Double = 50.0,
    val lightOffLux: Double = 200.0,
    val tempFanOnC: Double = 28.0,
    val tempFanOffC: Double = 26.0,
    val rainPct: Double = 30.0,
    val fanAutoSpeed: Int = 70,
    val autoWindowReopen: Boolean = false,
    val enabled: Boolean = true,
) {
    // 阈值拼成 JSON
    fun toJson(): String = buildString {
        append('{')
        append("\"light_on_lux\":").append(num(lightOnLux)).append(',')
        append("\"light_off_lux\":").append(num(lightOffLux)).append(',')
        append("\"temp_fan_on_c\":").append(num(tempFanOnC)).append(',')
        append("\"temp_fan_off_c\":").append(num(tempFanOffC)).append(',')
        append("\"rain_pct\":").append(num(rainPct)).append(',')
        append("\"fan_auto_speed\":").append(fanAutoSpeed.coerceIn(0, 100)).append(',')
        append("\"auto_window_reopen\":").append(autoWindowReopen).append(',')
        append("\"enabled\":").append(enabled)
        append('}')
    }

    // 整数不带小数点
    private fun num(v: Double): String =
        if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()
}
