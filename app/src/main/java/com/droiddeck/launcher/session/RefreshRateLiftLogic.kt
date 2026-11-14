package com.droiddeck.launcher.session

import kotlin.math.abs

/**
 * [mali-g2] What [RefreshRateLift] does with HyperOS's refresh rate setting for apps (Secure user_refresh_rate), without
 * Android: [secure] reads and writes the setting, [records] keeps what this app wrote. Every call returns a line for the
 * log, or null when there is nothing to tell. A call that throws (the setting could not be read or written) leaves the
 * record as it was, so the next call tries again.
 *
 * While a session activity is shown and the setting is on, a setting below the panel's top rate is raised to it; the
 * value before is recorded first (null when it was unset). It is put back when no session activity is shown any more,
 * the session ends or the setting is turned off, and on the next app or session start while none is shown (the process
 * died with it raised), but only while the setting still holds the value written here: otherwise the owner changed it
 * meanwhile, and only the record goes.
 */
internal class RefreshRateLiftLogic(private val secure: Secure, private val records: Records) {
    interface Secure {
        /** The current value, null when unset; throws when it cannot be read. */
        fun read(): String?
        fun put(value: String)
        fun delete()
    }

    interface Records {
        fun load(): Record?
        fun save(record: Record)
        fun clear()
    }

    /** [written] was put in place of [original] (null: it was unset). */
    data class Record(val original: String?, val written: String)

    /** A session activity is shown. */
    var visible = false
        private set

    fun shown(enabled: Boolean, topHz: Int): String? {
        visible = true
        return if (enabled) lift(topHz) else restore()
    }

    fun hidden(): String? {
        visible = false
        return restore()
    }

    fun sessionStarted(enabled: Boolean, topHz: Int): String? = if (visible) shown(enabled, topHz) else restore()

    fun sessionEnded(): String? = restore()

    fun appStarted(): String? = if (visible) null else restore()

    fun turnedOff(): String? = restore()

    private fun lift(topHz: Int): String? {
        if (topHz <= 0) return null
        val record = records.load()?.let { if (valid(it)) it else { records.clear(); null } }
        val current = secure.read()
        if (record != null && same(current, record.written)) return null
        val changed = if (record != null) ", changed from ${record.written} meanwhile" else ""
        if (!below(current, topHz)) {
            if (record != null) records.clear()
            return "$KEY $current$changed: already reaches the panel's $topHz Hz"
        }
        val next = Record(current, topHz.toString())
        records.save(next)
        secure.put(next.written)
        return "$KEY ${current ?: "unset"}$changed -> ${next.written} while a session is on screen"
    }

    private fun restore(): String? {
        val record = records.load() ?: return null
        if (!valid(record)) {
            records.clear()
            return "record of $KEY not understood (${record.original} -> ${record.written}): dropped"
        }
        val current = secure.read()
        if (!same(current, record.written)) {
            records.clear()
            return "$KEY is ${current ?: "unset"}, not the ${record.written} written here: left as it is"
        }
        if (record.original == null) secure.delete() else secure.put(record.original)
        records.clear()
        return "$KEY ${record.written} -> ${record.original ?: "unset"} again"
    }

    companion object {
        const val KEY = "user_refresh_rate"
        private val VALUE = Regex("[0-9]{1,4}(\\.[0-9]{1,4})?")

        /** Whether [value] is a rate as the setting holds it, so fit for a shell command line. */
        fun isRate(value: String) = VALUE.matches(value)

        /** The output of `settings get`: null when unset; throws on anything but a rate. */
        fun parse(output: String): String? {
            val value = output.trim()
            if (value == "null") return null
            require(VALUE.matches(value)) { "unexpected $KEY '${value.take(40)}'" }
            return value
        }

        /** Whether [current] (null: unset) is below [topHz], so a session would raise it. */
        fun below(current: String?, topHz: Int): Boolean {
            if (topHz <= 0) return false
            val rate = current?.toDoubleOrNull() ?: return current == null
            return rate < topHz - 0.5
        }

        internal fun same(a: String?, b: String?): Boolean {
            if (a == null || b == null) return a == b
            val x = a.toDoubleOrNull()
            val y = b.toDoubleOrNull()
            return if (x != null && y != null) abs(x - y) < 0.01 else a == b
        }

        internal fun valid(record: Record) =
            VALUE.matches(record.written) && (record.original == null || VALUE.matches(record.original))
    }
}
