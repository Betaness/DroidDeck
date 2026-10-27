package com.droiddeck.launcher.session.play

/**
 * [mali-g2] The play journal's JSON: objects are [LinkedHashMap]s (key order kept), arrays [List]s.
 *
 * Not org.json: in plain JVM unit tests that is Android's stub, and its key order is not ours to fix.
 * Numbers read back as Long when integral, else Double.
 */
object Json {
    fun write(value: Any?): String = StringBuilder().also { append(it, value) }.toString()

    private fun append(out: StringBuilder, value: Any?) {
        when (value) {
            null -> out.append("null")
            is String -> quote(out, value)
            is Boolean -> out.append(value)
            is Int, is Long, is Short, is Byte -> out.append(value.toString())
            is Float -> number(out, value.toDouble())
            is Double -> number(out, value)
            is Number -> number(out, value.toDouble())
            is Map<*, *> -> {
                out.append('{')
                var first = true
                for ((k, v) in value) {
                    if (!first) out.append(',')
                    first = false
                    quote(out, k.toString())
                    out.append(':')
                    append(out, v)
                }
                out.append('}')
            }
            is Iterable<*> -> {
                out.append('[')
                var first = true
                for (v in value) {
                    if (!first) out.append(',')
                    first = false
                    append(out, v)
                }
                out.append(']')
            }
            is IntArray -> append(out, value.asList())
            is LongArray -> append(out, value.asList())
            is FloatArray -> append(out, value.asList())
            is DoubleArray -> append(out, value.asList())
            is Array<*> -> append(out, value.asList())
            else -> quote(out, value.toString())
        }
    }

    private fun number(out: StringBuilder, d: Double) {
        if (d.isNaN() || d.isInfinite()) { out.append("null"); return }
        if (d == Math.rint(d) && Math.abs(d) < 1e15) out.append(d.toLong()) else out.append(d)
    }

    private fun quote(out: StringBuilder, s: String) {
        out.append('"')
        for (c in s) {
            when (c) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else -> if (c < ' ' || c == ' ' || c == ' ') {
                    out.append("\\u").append(String.format(java.util.Locale.US, "%04x", c.code))
                } else out.append(c)
            }
        }
        out.append('"')
    }

    /** Parses one JSON value; throws [IllegalArgumentException] on malformed text. */
    fun parse(text: String): Any? {
        val p = Parser(text)
        p.ws()
        val v = p.value()
        p.ws()
        require(p.i == text.length) { "trailing text at ${p.i}" }
        return v
    }

    fun parseOrNull(text: String): Any? = try { parse(text) } catch (_: Exception) { null }

    private class Parser(val s: String) {
        var i = 0

        fun ws() { while (i < s.length && s[i].isWhitespace()) i++ }

        fun value(): Any? {
            require(i < s.length) { "unexpected end" }
            return when (s[i]) {
                '{' -> obj()
                '[' -> arr()
                '"' -> str()
                't' -> word("true", true)
                'f' -> word("false", false)
                'n' -> word("null", null)
                else -> num()
            }
        }

        private fun word(w: String, v: Any?): Any? {
            require(s.startsWith(w, i)) { "bad literal at $i" }
            i += w.length
            return v
        }

        private fun obj(): Map<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            i++
            ws()
            if (i < s.length && s[i] == '}') { i++; return m }
            while (true) {
                ws()
                require(i < s.length && s[i] == '"') { "key expected at $i" }
                val k = str()
                ws()
                require(i < s.length && s[i] == ':') { "':' expected at $i" }
                i++
                ws()
                m[k] = value()
                ws()
                require(i < s.length) { "unexpected end" }
                if (s[i] == ',') { i++; continue }
                require(s[i] == '}') { "'}' expected at $i" }
                i++
                return m
            }
        }

        private fun arr(): List<Any?> {
            val l = ArrayList<Any?>()
            i++
            ws()
            if (i < s.length && s[i] == ']') { i++; return l }
            while (true) {
                ws()
                l.add(value())
                ws()
                require(i < s.length) { "unexpected end" }
                if (s[i] == ',') { i++; continue }
                require(s[i] == ']') { "']' expected at $i" }
                i++
                return l
            }
        }

        private fun str(): String {
            i++
            val b = StringBuilder()
            while (true) {
                require(i < s.length) { "unterminated string" }
                val c = s[i++]
                when (c) {
                    '"' -> return b.toString()
                    '\\' -> {
                        require(i < s.length) { "bad escape" }
                        when (val e = s[i++]) {
                            '"', '\\', '/' -> b.append(e)
                            'b' -> b.append('\b')
                            'f' -> b.append('\u000c')
                            'n' -> b.append('\n')
                            'r' -> b.append('\r')
                            't' -> b.append('\t')
                            'u' -> {
                                require(i + 4 <= s.length) { "bad \\u escape" }
                                b.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> throw IllegalArgumentException("bad escape \\$e")
                        }
                    }
                    else -> b.append(c)
                }
            }
        }

        private fun num(): Any {
            val start = i
            if (i < s.length && (s[i] == '-' || s[i] == '+')) i++
            while (i < s.length && (s[i].isDigit() || s[i] in ".eE+-")) i++
            val t = s.substring(start, i)
            require(t.isNotEmpty()) { "value expected at $start" }
            return if (t.any { it in ".eE" }) t.toDouble() else t.toLongOrNull() ?: t.toDouble()
        }
    }
}

/** Typed reads from parsed JSON maps. */
internal fun Map<*, *>.str(key: String): String? = this[key] as? String
internal fun Map<*, *>.long(key: String): Long? = (this[key] as? Number)?.toLong()
internal fun Map<*, *>.dbl(key: String): Double? = (this[key] as? Number)?.toDouble()
internal fun Map<*, *>.obj(key: String): Map<*, *>? = this[key] as? Map<*, *>
internal fun Map<*, *>.list(key: String): List<*>? = this[key] as? List<*>
