package com.smarthome.ble.data

/**
 * A deliberately tiny, tolerant JSON reader.
 *
 * The project asked for minimal dependencies, so instead of pulling in
 * kotlinx.serialization / Gson / Moshi we hand-roll just enough JSON parsing.
 *
 * Guarantees that matter for this app:
 *  - Unknown / extra keys are ignored (never an error).
 *  - Missing keys return `null` (callers fall back to defaults).
 *  - A malformed payload yields `null` instead of throwing.
 *  - Type coercion: a number sent as a JSON string (or vice versa) still parses,
 *    because firmware teams change their minds about quoting.
 */
object Json {

    const val TAG = "SmartHomeJson"

    /** Parses a whole document. Returns null when the text is not a JSON object. */
    fun parseObject(text: String): MutableMap<String, Any?>? {
        return try {
            val p = Parser(text)
            p.skipWs()
            val v = p.readValue()
            if (v is Map<*, *>) {
                @Suppress("UNCHECKED_CAST")
                v as MutableMap<String, Any?>
            } else null
        } catch (_: Throwable) {
            null
        }
    }

    /** Reads a raw value (object / array / string / number / bool / null) by key. */
    fun raw(obj: Map<String, Any?>?, key: String): Any? = obj?.get(key)

    /** Nested object access. */
    fun obj(obj: Map<String, Any?>?, key: String): Map<String, Any?>? {
        val v = obj?.get(key) ?: return null
        @Suppress("UNCHECKED_CAST")
        return v as? Map<String, Any?>
    }

    /** Number access, tolerant of ints, doubles and numeric strings. */
    fun num(obj: Map<String, Any?>?, key: String): Double? {
        val v = obj?.get(key) ?: return null
        return toDouble(v)
    }

    fun toDouble(v: Any?): Double? = when (v) {
        null -> null
        is Double -> if (v.isNaN() || v.isInfinite()) null else v
        is Number -> v.toDouble()
        is Boolean -> if (v) 1.0 else 0.0
        is String -> v.trim().toDoubleOrNull()
        else -> null
    }

    fun int(obj: Map<String, Any?>?, key: String, default: Int = 0): Int =
        num(obj, key)?.let { Math.round(it).toInt() } ?: default

    fun dbl(obj: Map<String, Any?>?, key: String, default: Double = 0.0): Double =
        num(obj, key) ?: default

    fun str(obj: Map<String, Any?>?, key: String): String? = when (val v = obj?.get(key)) {
        null -> null
        is String -> v
        is Number -> if (v.toDouble() == v.toLong().toDouble()) v.toLong().toString() else v.toString()
        is Boolean -> v.toString()
        else -> null
    }

    /**
     * Boolean access. Also accepts 0/1 and "true"/"false" because firmware
     * sometimes emits `"power": 1`.
     */
    fun bool(obj: Map<String, Any?>?, key: String): Boolean? {
        val v = obj?.get(key) ?: return null
        return toBool(v)
    }

    fun toBool(v: Any?): Boolean? = when (v) {
        null -> null
        is Boolean -> v
        is Number -> v.toDouble() != 0.0
        is String -> when (v.trim().lowercase()) {
            "true", "yes", "on", "1" -> true
            "false", "no", "off", "0", "" -> false
            else -> null
        }
        else -> null
    }

    fun boolOf(obj: Map<String, Any?>?, key: String, default: Boolean = false): Boolean =
        bool(obj, key) ?: default

    // ------------------------------------------------------------- internals --

    private class Parser(private val s: String) {
        private var i = 0

        fun skipWs() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        fun readValue(): Any? {
            skipWs()
            if (i >= s.length) throw IllegalStateException("eof")
            return when (val c = s[i]) {
                '{' -> readObject()
                '[' -> readArray()
                '"' -> readString()
                't', 'f' -> readKeyword()
                'n' -> {
                    expect("null"); null
                }
                else -> {
                    if (c == '-' || c == '+' || c.isDigit() || c == '.') readNumber()
                    else throw IllegalStateException("unexpected '$c' at $i")
                }
            }
        }

        private fun readObject(): MutableMap<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            i++ // '{'
            skipWs()
            if (i < s.length && s[i] == '}') { i++; return m }
            while (true) {
                skipWs()
                if (i >= s.length) throw IllegalStateException("eof in object")
                if (s[i] == '}') { i++; break }
                if (s[i] == ',') { i++; continue }
                if (s[i] != '"') throw IllegalStateException("key must be a string at $i")
                val k = readString()
                skipWs()
                if (i >= s.length || s[i] != ':') throw IllegalStateException("missing ':' at $i")
                i++
                m[k] = readValue()
                skipWs()
                if (i < s.length && s[i] == ',') { i++; continue }
                if (i < s.length && s[i] == '}') { i++; break }
            }
            return m
        }

        private fun readArray(): MutableList<Any?> {
            val l = ArrayList<Any?>()
            i++ // '['
            skipWs()
            if (i < s.length && s[i] == ']') { i++; return l }
            while (true) {
                l.add(readValue())
                skipWs()
                if (i >= s.length) break
                if (s[i] == ',') { i++; continue }
                if (s[i] == ']') { i++; break }
                throw IllegalStateException("bad array at $i")
            }
            return l
        }

        private fun readString(): String {
            i++ // opening quote
            val sb = StringBuilder()
            while (true) {
                if (i >= s.length) throw IllegalStateException("eof in string")
                val c = s[i++]
                when (c) {
                    '"' -> return sb.toString()
                    '\\' -> {
                        if (i >= s.length) throw IllegalStateException("eof in escape")
                        when (val e = s[i++]) {
                            '"' -> sb.append('"')
                            '\\' -> sb.append('\\')
                            '/' -> sb.append('/')
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000C')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) throw IllegalStateException("bad \\u")
                                val hex = s.substring(i, i + 4)
                                i += 4
                                sb.append(hex.toInt(16).toChar())
                            }
                            else -> sb.append(e)
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        private fun readNumber(): Double {
            val start = i
            while (i < s.length && (s[i].isDigit() || s[i] == '-' || s[i] == '+' ||
                        s[i] == '.' || s[i] == 'e' || s[i] == 'E')
            ) i++
            val t = s.substring(start, i)
            return t.toDoubleOrNull() ?: throw IllegalStateException("bad number '$t'")
        }

        private fun readKeyword(): Boolean {
            if (s.startsWith("true", i)) { i += 4; return true }
            if (s.startsWith("false", i)) { i += 5; return false }
            throw IllegalStateException("bad keyword at $i")
        }

        private fun expect(word: String) {
            if (!s.startsWith(word, i)) throw IllegalStateException("expected $word at $i")
            i += word.length
        }
    }
}
