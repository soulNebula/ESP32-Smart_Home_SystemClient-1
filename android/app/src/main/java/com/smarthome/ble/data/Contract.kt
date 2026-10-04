package com.smarthome.ble.data

import java.nio.charset.StandardCharsets
import java.util.UUID

/**
 * The frozen wire contract between this app and the ESP32-S3 firmware.
 *
 * EVERYTHING in this file mirrors the firmware exactly. Do not "improve" the
 * UUIDs, the type codes or the JSON key names -- the firmware team owns them.
 */
object Proto {

    // ---------------------------------------------------------------- naming --
    /** Advertised name is `SmartHome-<uid>` with a per-board uid, e.g. SmartHome-4d4a64. */
    const val NAME_PREFIX = "SmartHome-"

    // ----------------------------------------------------------------- UUIDs --
    val SERVICE_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d")

    /** Phone -> board. Write / Write-Without-Response. */
    val RX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5e")

    /** Board -> phone. Notify. */
    val TX_CHAR_UUID: UUID = UUID.fromString("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5f")

    /** Standard Client Characteristic Configuration Descriptor. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // ------------------------------------------------------------- frame types --
    /** Phone -> board type codes. */
    object Down {
        const val CMD: Byte = 0x01
        const val CONFIG: Byte = 0x02
        const val GET: Byte = 0x03
    }

    /** Board -> phone type codes. */
    object Up {
        const val STATE: Byte = 0x01
        const val SENSOR: Byte = 0x02
        const val ACK: Byte = 0x03
        const val EVENT: Byte = 0x04
        const val CONFIG: Byte = 0x05
    }

    // ------------------------------------------------------------- devices --
    const val DEV_ALL = "all"
    const val DEV_LED_LIVING = "led_living"
    const val DEV_LED_KITCHEN = "led_kitchen"
    const val DEV_LED_BEDROOM = "led_bedroom"
    const val DEV_LED_BATH = "led_bath"
    const val DEV_FAN = "fan"
    const val DEV_WINDOW = "window"
    const val DEV_DOOR = "door"
    const val DEV_CURTAIN = "curtain"

    /** The four lamp ids, in UI order. */
    val LAMPS = listOf(DEV_LED_LIVING, DEV_LED_KITCHEN, DEV_LED_BEDROOM, DEV_LED_BATH)

    /** The three open/close actuators, in UI order. */
    val ACTUATORS = listOf(DEV_WINDOW, DEV_DOOR, DEV_CURTAIN)

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

    // ------------------------------------------------------------- framing --
    /**
     * Builds a frame: `byte[0] = type`, `byte[1..] = UTF-8 JSON`.
     * Both directions use this identical layout.
     */
    fun frame(type: Byte, json: String): ByteArray {
        val body = json.toByteArray(StandardCharsets.UTF_8)
        val out = ByteArray(body.size + 1)
        out[0] = type
        System.arraycopy(body, 0, out, 1, body.size)
        return out
    }

    /** Decodes `byte[1..]` of a received frame into a JSON string. */
    fun payloadText(frame: ByteArray): String {
        if (frame.size <= 1) return ""
        return String(frame, 1, frame.size - 1, StandardCharsets.UTF_8)
    }

    // ------------------------------------------------------- command builders --
    fun cmd(dev: String, action: String): String = """{"dev":"$dev","action":"$action"}"""

    fun cmdSet(dev: String, value: Int): String =
        """{"dev":"$dev","action":"set","value":${value.coerceIn(0, 100)}}"""

    fun cmdColor(dev: String, r: Int, g: Int, b: Int): String =
        """{"dev":"$dev","action":"color","r":${r.coerceIn(0, 255)},""" +
            """"g":${g.coerceIn(0, 255)},"b":${b.coerceIn(0, 255)}}"""

    fun cmdAuto(auto: Boolean): String = """{"action":"auto","value":$auto}"""

    /** Requests one immediate state + sensor push from the board. Type code 0x03. */
    fun cmdGet(): String = """{"get":"state"}"""
}

/**
 * Client-side settings thresholds (down type code 0x02).
 * Kept permissive: every field has a default matching the firmware defaults.
 */
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

    private fun num(v: Double): String =
        if (v == v.toLong().toDouble()) v.toLong().toString() else v.toString()
}
