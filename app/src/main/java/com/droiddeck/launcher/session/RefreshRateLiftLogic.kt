package com.droiddeck.launcher.session

import kotlin.math.abs

/**
 * [mali-g2] What [RefreshRateLift] does with HyperOS's refresh rate setting for apps (Secure user_refresh_rate), without
 * Android: [secure] reads and writes the setting and the restorer's marker, [records] keeps what this app wrote. Every
 * call returns a line for the log, or null when there is nothing to tell. A call that throws (the setting could not be
 * read or written) leaves the record as it was, so a later call tries again.
 *
 * While a session activity is on screen and the setting is on, a setting below the panel's top rate is raised to it.
 * Before the write the value before is recorded (null when it was unset) and a restorer is started as the shell user
 * ([armCommand]): it outlives this process and, once the process is gone, puts the value back if the setting still
 * holds the value written, then notes in its marker what it did. The record is marked confirmed once the write went
 * through.
 *
 * This app puts the value back when no session activity is on screen any more, the session ends or the setting is
 * turned off, and on the next app or session start while none is on screen; only while the setting still holds the
 * value written here, the restorer has not acted on it, and the write was confirmed or started by this process.
 * Otherwise the setting was changed meanwhile, and only the record goes. A record lost with the app's data is taken
 * from the marker while the setting still holds the value written and no restorer acted on it.
 */
internal class RefreshRateLiftLogic(
    private val secure: Secure,
    private val records: Records,
    private val newId: () -> String,
) {
    interface Secure {
        /** The current value, null when unset; throws when it cannot be read. */
        fun read(): String?
        fun put(value: String)
        /** Puts [original] in place (deletes the setting for null) and removes the marker, which stops the restorer. */
        fun putBack(original: String?)
        /** What the restorer's marker holds, null when there is none. */
        fun marker(): Marker?
        /** Writes the marker for [record] and starts a restorer watching this process; an earlier restorer stops. */
        fun arm(record: Record)
        /** Removes the marker, which stops the restorer. */
        fun disarm()
    }

    interface Records {
        fun load(): Record?
        fun save(record: Record)
        fun clear()
    }

    /** [written] was put in place of [original] (null: it was unset), watched by the restorer [id]; [confirmed] once the write went through. */
    data class Record(val original: String?, val written: String, val id: String, val confirmed: Boolean = true)

    /** The restorer's marker: the record it watches and, once the process it watched is gone, what it did ([RESTORED] or [LEFT]). */
    data class Marker(val id: String, val original: String?, val written: String, val outcome: String?)

    /** A session activity is shown. */
    var visible = false
        private set

    /** The record whose write this process started, until it is confirmed. */
    private var attempted: Record? = null

    /** The restorer this process started, while its record stands. */
    private var armed: String? = null

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

    /** After a call failed: puts the value back when it is not wanted raised. */
    fun retry(enabled: Boolean): String? = if (visible && enabled) null else restore()

    /** Whether a value raised here waits to be put back. */
    fun pending(enabled: Boolean): Boolean = !(visible && enabled) && records.load() != null

    private fun lift(topHz: Int): String? {
        if (topHz <= 0) return null
        val loaded = records.load()?.let { if (valid(it)) it else { records.clear(); null } }
        val current = secure.read()
        if (loaded != null && loaded.id == armed && loaded.confirmed && same(current, loaded.written)) return null
        val marker = secure.marker()
        val record = loaded ?: adopted(marker, current)
        val stale = record?.let { stale(it, current, marker) }
        if (record != null && stale == null) {
            val next = record.copy(id = newId(), confirmed = true)
            records.save(next)
            secure.arm(next)
            armed = next.id
            attempted = null
            val from = if (loaded == null) "recorded only in the restorer's marker" else "raised before"
            return "$KEY ${record.written} ${from}: kept, the restorer now watches this process"
        }
        val note = stale?.let { ", $it" } ?: ""
        if (!below(current, topHz)) {
            if (loaded != null) {
                secure.disarm()
                records.clear()
                forget()
            }
            return "$KEY ${current ?: "unset"}$note: already reaches the panel's $topHz Hz"
        }
        val next = Record(current, topHz.toString(), newId(), confirmed = false)
        records.save(next)
        attempted = next
        try {
            secure.arm(next)
        } catch (t: Throwable) {
            records.clear()
            attempted = null
            throw t
        }
        armed = next.id
        secure.put(next.written)
        records.save(next.copy(confirmed = true))
        attempted = null
        return "$KEY ${current ?: "unset"}$note -> ${next.written} while a session is on screen"
    }

    private fun restore(): String? {
        val record = records.load() ?: return null
        if (!valid(record)) {
            records.clear()
            return "record of $KEY not understood (${record.original} -> ${record.written}): dropped"
        }
        val current = secure.read()
        // This process's own restorer has not acted: it acts only once the process is gone.
        val stale = stale(record, current, if (record.id == armed) null else secure.marker())
        if (stale != null) {
            secure.disarm()
            records.clear()
            forget()
            return "$KEY ${current ?: "unset"}, $stale: left as it is"
        }
        secure.putBack(record.original)
        records.clear()
        forget()
        return "$KEY ${record.written} -> ${record.original ?: "unset"} again"
    }

    private fun forget() {
        armed = null
        attempted = null
    }

    /** Why [record] is not this app's to put back any more, or null while it is. */
    private fun stale(record: Record, current: String?, marker: Marker?): String? {
        val acted = marker?.takeIf { it.id == record.id }?.outcome
        return when {
            !same(current, record.written) -> "changed from ${record.written} meanwhile"
            acted == RESTORED -> "put back by the restorer and set to ${record.written} again since"
            acted != null -> "changed and set to ${record.written} again since"
            !record.confirmed && record != attempted -> "the write of ${record.written} was not confirmed"
            else -> null
        }
    }

    /** The record in a marker no restorer acted on, while the setting holds the value written: the app's own record went with its data. */
    private fun adopted(marker: Marker?, current: String?): Record? {
        if (marker == null || marker.outcome != null || !same(current, marker.written)) return null
        return Record(marker.original, marker.written, marker.id).takeIf(::valid)
    }

    companion object {
        const val KEY = "user_refresh_rate"
        const val RESTORED = "restored"
        const val LEFT = "left"
        private const val UNSET = "unset"
        /** How often the restorer looks whether the process is gone, in seconds. */
        const val POLL_S = 3
        private val VALUE = Regex("[0-9]{1,4}(\\.[0-9]{1,4})?")
        private val ID = Regex("[0-9a-z]{1,32}")
        private val PACKAGE = Regex("[A-Za-z0-9._]+")

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
            VALUE.matches(record.written) && (record.original == null || VALUE.matches(record.original)) && ID.matches(record.id)

        /** The marker's file for [packageName], in the shell user's /data/local/tmp. */
        fun markerPath(packageName: String): String {
            require(PACKAGE.matches(packageName)) { "unexpected package '$packageName'" }
            return "/data/local/tmp/refresh-lift.$packageName"
        }

        /** The marker's line for [record]. */
        fun markerLine(record: Record) = "${record.id} ${record.original ?: UNSET} ${record.written}"

        /** The marker file's contents; null when empty or not understood. */
        fun parseMarker(text: String): Marker? {
            val words = text.trim().split(Regex("\\s+"))
            if (words.size !in 3..4) return null
            val (id, original, written) = words
            val outcome = words.getOrNull(3)
            if (!ID.matches(id) || !VALUE.matches(written) || (original != UNSET && !VALUE.matches(original))) return null
            if (outcome != null && outcome != RESTORED && outcome != LEFT) return null
            return Marker(id, original.takeIf { it != UNSET }, written, outcome)
        }

        /** The shell line that puts [original] back and removes the marker [file]. */
        fun putBackCommand(file: String, original: String?): String {
            require(original == null || isRate(original)) { "not a rate: '$original'" }
            val put = if (original == null) "settings delete secure $KEY" else "settings put secure $KEY $original"
            return "$put && rm -f $file"
        }

        /**
         * The shell line that writes the marker [file] for [record] and starts its restorer, detached so it outlives the
         * connection and process [pid]. Every [pollSeconds] it looks whether [pid] is still there; it stops when the
         * marker is gone or names another restorer. Once [pid] is gone it puts the value before back if the setting
         * still holds the value written, and notes [RESTORED] or [LEFT] in the marker.
         */
        fun armCommand(file: String, pid: Int, record: Record, pollSeconds: Int = POLL_S): String {
            require(valid(record) && pid > 0 && pollSeconds > 0 && file.matches(Regex("[A-Za-z0-9._/-]+"))) { "cannot arm $record" }
            val line = markerLine(record)
            val putBack = if (record.original == null) "settings delete secure $KEY" else "settings put secure $KEY ${record.original}"
            val restorer = "while [ -d /proc/$pid ]; do sleep $pollSeconds; " +
                "read -r a r < $file || exit 0; [ \"\$a\" = ${record.id} ] || exit 0; done; " +
                "read -r a o w r < $file || exit 0; [ \"\$a\" = ${record.id} ] && [ -z \"\$r\" ] || exit 0; " +
                "if [ \"\$(settings get secure $KEY)\" = ${record.written} ]; then $putBack && echo $line $RESTORED > $file; " +
                "else echo $line $LEFT > $file; fi"
            return "echo $line > $file.tmp && mv -f $file.tmp $file && " +
                "{ setsid sh -c '$restorer' </dev/null >/dev/null 2>&1 & }"
        }
    }
}
