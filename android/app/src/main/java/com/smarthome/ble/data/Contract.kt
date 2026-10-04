/*
 * 模块：
 *   协议约定。把手机和板子说好的东西全钉在这一个文件里：蓝牙服务号和两个特征号、
 *   包的第一个字节是什么意思、JSON 里字段叫什么名、八路设备叫什么。
 *   BleManager 拿它去收发，SmartHomeViewModel 和界面的名字也都从这儿取，
 *   写 JSON 的地方用 Json.kt。
 *   广播名前缀 NAME_PREFIX 必须和板子 Kconfig 里的 APP_BLE_DEVICE_NAME_PREFIX 对得上，
 *   板子那边广播名是「前缀-MAC后三字节」，所以这边要多带那个短横线，否则扫不到板子。
 *
 * 功能：
 *   定服务号和特征号
 *   定上下行类型码
 *   认八路设备名
 *   拼各种命令 JSON
 *   存阈值并转 JSON
 */
package com.smarthome.ble.data

import java.nio.charset.StandardCharsets
import java.util.UUID

object Proto {

    // 功能：板子广播名前缀
    const val NAME_PREFIX = "SmartHome-"

    val SERVICE_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d")

    // 功能：手机发给板子
    val RX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e")

    // 功能：板子推给手机
    val TX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f")

    // 功能：开关通知的那一格
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // 功能：手机往下的包号
    object Down {
        const val CMD: Byte = 0x01
        const val CONFIG: Byte = 0x02
        const val GET: Byte = 0x03
    }

    // 功能：板子往上的包号
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

    // 功能：四路灯的先后
    val LAMPS = listOf(DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH)

    // 功能：窗门帘的先后
    val ACTUATORS = listOf(DEV_WINDOW, DEV_DOOR, DEV_CURTAIN)

    /* 功能：把代号翻成中文 */
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

    /* 功能：首字节带包号 */
    fun frame(type: Byte, json: String): ByteArray {
        val body = json.toByteArray(StandardCharsets.UTF_8)
        val out = ByteArray(body.size + 1)
        out[0] = type
        System.arraycopy(body, 0, out, 1, body.size)
        return out
    }

    /* 功能：去掉包号留下文字 */
    fun payloadText(frame: ByteArray): String {
        if (frame.size <= 1) return ""
        return String(frame, 1, frame.size - 1, StandardCharsets.UTF_8)
    }

    /* 功能：拼开关命令 */
    fun cmd(dev: String, action: String): String = """{"dev":"$dev","action":"$action"}"""

    /* 功能：拼调值命令 */
    fun cmdSet(dev: String, value: Int): String =
        """{"dev":"$dev","action":"set","value":${value.coerceIn(0, 100)}}"""

    /* 功能：拼调色命令 */
    fun cmdColor(dev: String, r: Int, g: Int, b: Int): String =
        """{"dev":"$dev","action":"color","r":${r.coerceIn(0, 255)},""" +
            """"g":${g.coerceIn(0, 255)},"b":${b.coerceIn(0, 255)}}"""

    /* 功能：拼自动模式命令 */
    fun cmdAuto(auto: Boolean): String = """{"action":"auto","value":$auto}"""

    /* 功能：拼要一次状态 */
    fun cmdGet(): String = """{"get":"state"}"""
}

// 功能：存自动模式的阈值
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
    /* 功能：阈值拼成 JSON */
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

    /* 功能：整数不带小数点 */
    private fun num(v: Double): String =
        if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()
}
