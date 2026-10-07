package com.smarthome.ble.data

object Json {

    const val TAG = "SmartHomeJson"

    // 整段解成对象
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

    // 原样取一个值
    fun raw(obj: Map<String, Any?>?, key: String): Any? = obj?.get(key)

    // 取里层对象
    fun obj(obj: Map<String, Any?>?, key: String): Map<String, Any?>? {
        val v = obj?.get(key) ?: return null
        @Suppress("UNCHECKED_CAST")
        return v as? Map<String, Any?>
    }

    // 取一个数
    fun num(obj: Map<String, Any?>?, key: String): Double? {
        val v = obj?.get(key) ?: return null
        return toDouble(v)
    }

    // 什么写法都认
    fun toDouble(v: Any?): Double? = when (v) {
        null -> null
        is Double -> if (v.isNaN() || v.isInfinite()) null else v
        is Number -> v.toDouble()
        is Boolean -> if (v) 1.0 else 0.0
        is String -> v.trim().toDoubleOrNull()
        else -> null
    }

    // 取整数
    fun int(obj: Map<String, Any?>?, key: String, default: Int = 0): Int =
        num(obj, key)?.let { Math.round(it).toInt() } ?: default

    // 取小数
    fun dbl(obj: Map<String, Any?>?, key: String, default: Double = 0.0): Double =
        num(obj, key) ?: default

    // 取一段文字
    fun str(obj: Map<String, Any?>?, key: String): String? = when (val v = obj?.get(key)) {
        null -> null
        is String -> v
        is Number -> if (v.toDouble() == v.toLong().toDouble()) v.toLong().toString() else v.toString()
        is Boolean -> v.toString()
        else -> null
    }

    // 取开关值
    fun bool(obj: Map<String, Any?>?, key: String): Boolean? {
        val v = obj?.get(key) ?: return null
        return toBool(v)
    }

    // 数字文字都当开关
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

    // 取不开就默认值
    fun boolOf(obj: Map<String, Any?>?, key: String, default: Boolean = false): Boolean =
        bool(obj, key) ?: default

    private class Parser(private val s: String) {
        private var i = 0

        // 跳过空白
        fun skipWs() {
            while (i < s.length && s[i].isWhitespace()) i++
        }

        // 读下一个值
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

        // 读一个大括号
        private fun readObject(): MutableMap<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            // 跳过左括号
            i++
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

        // 读一个方括号
        private fun readArray(): MutableList<Any?> {
            val l = ArrayList<Any?>()
            // 跳过左方括号
            i++
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

        // 读一对引号
        private fun readString(): String {
            // 跳过开引号
            i++
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

        // 读一个数
        private fun readNumber(): Double {
            val start = i
            while (i < s.length && (s[i].isDigit() || s[i] == '-' || s[i] == '+' ||
                        s[i] == '.' || s[i] == 'e' || s[i] == 'E')
            ) i++
            val t = s.substring(start, i)
            return t.toDoubleOrNull() ?: throw IllegalStateException("bad number '$t'")
        }

        // 读 true 或 false
        private fun readKeyword(): Boolean {
            if (s.startsWith("true", i)) { i += 4; return true }
            if (s.startsWith("false", i)) { i += 5; return false }
            throw IllegalStateException("bad keyword at $i")
        }

        // 对一下是不是这个词
        private fun expect(word: String) {
            if (!s.startsWith(word, i)) throw IllegalStateException("expected $word at $i")
            i += word.length
        }
    }
}
