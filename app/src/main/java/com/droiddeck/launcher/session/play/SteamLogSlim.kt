package com.droiddeck.launcher.session.play

import java.io.File
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

/**
 * [mali-g2] Cuts a mirrored session folder's Steam logs (steam/) to the session's time window.
 *
 * Steam keeps appending to the same logs for days, and the session copies them whole: in one 80 MB folder
 * 73 MB was steam/, 927 KB of it from the session. The window is the session's events.jsonl, from 10 minutes
 * before its first event to 2 minutes after its last. Lines stamped `[YYYY-MM-DD HH:MM:SS]` (local time) are
 * kept inside it, unstamped lines follow the line before; logs without stamps keep their last 256 KB;
 * the `.previous` files go. `.slim` records what was done.
 */
object SteamLogSlim {
    const val MARKER = ".slim"
    const val TAIL_BYTES = 256L * 1024
    private const val BEFORE_MS = 10 * 60_000L
    private const val AFTER_MS = 2 * 60_000L
    private val STAMP = Regex("""^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})]""")
    private val EVENT_T = Regex("""^\{"t":(\d{10,})""")

    /** Slims [dir]; returns the bytes freed. */
    fun slim(dir: File, zone: TimeZone = TimeZone.getDefault()): Long {
        if (File(dir, MARKER).exists()) return 0L
        val steam = File(dir, "steam")
        val before = MirrorRetention.size(dir)
        val window = window(File(dir, "events.jsonl"))
        val notes = StringBuilder()
        if (steam.isDirectory) {
            steam.listFiles()?.filter { it.isFile }?.sortedBy { it.name }?.forEach { f ->
                val was = f.length()
                when {
                    f.name.contains(".previous") -> f.delete()
                    window != null && stamped(f) -> cut(f, window.first - BEFORE_MS, window.second + AFTER_MS, zone)
                    else -> tail(f, TAIL_BYTES)
                }
                notes.append(f.name).append(' ').append(was).append(" -> ").append(if (f.exists()) f.length() else 0).append('\n')
            }
        }
        val after = MirrorRetention.size(dir)
        try {
            File(dir, MARKER).writeText("slimmed: $before -> $after bytes" +
                (window?.let { " (window ${it.first}..${it.second})" } ?: " (no window)") + "\n" + notes)
        } catch (_: Exception) {
        }
        return (before - after).coerceAtLeast(0)
    }

    /** First and last event time of a session's events.jsonl. */
    fun window(events: File): Pair<Long, Long>? {
        if (!events.isFile) return null
        var first: Long? = null
        var last: Long? = null
        try {
            events.forEachLine { line ->
                EVENT_T.find(line)?.groupValues?.get(1)?.toLongOrNull()?.let { t ->
                    if (first == null) first = t
                    last = maxOf(last ?: t, t)
                }
            }
        } catch (_: Exception) {
            return null
        }
        val f = first ?: return null
        return f to (last ?: f)
    }

    private fun stamped(f: File): Boolean = try {
        f.bufferedReader().useLines { lines -> lines.take(50).any { STAMP.containsMatchIn(it) } }
    } catch (_: Exception) {
        false
    }

    /** Keeps the lines stamped within [fromMs, toMs] and the unstamped lines after them. */
    fun cut(f: File, fromMs: Long, toMs: Long, zone: TimeZone) {
        val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).apply { timeZone = zone; isLenient = false }
        val tmp = File(f.parentFile, ".${f.name}.slim")
        try {
            var keeping = false
            tmp.bufferedWriter().use { w ->
                f.forEachLine { line ->
                    val m = STAMP.find(line)
                    if (m != null) {
                        val t = try { fmt.parse(m.groupValues[1])?.time } catch (_: Exception) { null }
                        keeping = t != null && t in fromMs..toMs
                    }
                    if (keeping) { w.write(line); w.newLine() }
                }
            }
            if (!tmp.renameTo(f)) { tmp.copyTo(f, overwrite = true); tmp.delete() }
        } catch (_: Exception) {
            tmp.delete()
        }
    }

    /** Keeps the last [bytes] of [f], from a line start. */
    fun tail(f: File, bytes: Long) {
        if (f.length() <= bytes) return
        val tmp = File(f.parentFile, ".${f.name}.slim")
        try {
            java.io.RandomAccessFile(f, "r").use { raf ->
                raf.seek(f.length() - bytes)
                val buf = ByteArray(bytes.toInt())
                raf.readFully(buf)
                val nl = buf.indexOf('\n'.code.toByte())
                tmp.writeBytes(if (nl >= 0) buf.copyOfRange(nl + 1, buf.size) else buf)
            }
            if (!tmp.renameTo(f)) { tmp.copyTo(f, overwrite = true); tmp.delete() }
        } catch (_: Exception) {
            tmp.delete()
        }
    }
}
