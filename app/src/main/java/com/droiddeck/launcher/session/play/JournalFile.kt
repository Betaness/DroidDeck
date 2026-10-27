package com.droiddeck.launcher.session.play

import java.io.File

/**
 * [mali-g2] The play journal on disk: getExternalFilesDir()/journal/, readable with `adb shell` and outside the
 * session-log rotation.
 * - plays.jsonl: one JSON object per line, appended. Past [capBytes] it is rewritten with the newest whole
 *   lines that fit in [keepBytes] (records are 2-4 KB: over a thousand plays).
 * - open.json: the plays in progress, rewritten every minute, so a play the app did not live to finish is
 *   recorded at the next start.
 *
 * Every line goes through [redact] (LogRedactor.redact) like the other logs: string values one by one first,
 * then the whole line, which is kept when it still parses.
 */
class JournalFile(
    val dir: File,
    private val redact: (String) -> String,
    private val capBytes: Long = 5L * 1024 * 1024,
    private val keepBytes: Long = 4L * 1024 * 1024,
) {
    val plays = File(dir, "plays.jsonl")
    val open = File(dir, "open.json")

    fun line(record: Map<String, Any?>): String {
        val values = Json.write(redactValues(record))
        val whole = redact(values)
        return if (whole == values || Json.parseOrNull(whole) == null) values else whole
    }

    @Suppress("UNCHECKED_CAST")
    private fun redactValues(v: Any?): Any? = when (v) {
        is String -> redact(v)
        is Map<*, *> -> LinkedHashMap<String, Any?>().apply { v.forEach { (k, x) -> put(k.toString(), redactValues(x)) } }
        is Iterable<*> -> v.map { redactValues(it) }
        else -> v
    }

    @Synchronized
    fun append(record: Map<String, Any?>) {
        dir.mkdirs()
        val text = line(record) + "\n"
        plays.appendText(text, Charsets.UTF_8)
        if (plays.length() > capBytes) trim()
    }

    @Synchronized
    fun trim() {
        val lines = plays.readLines(Charsets.UTF_8)
        var size = 0L
        var from = lines.size
        while (from > 0) {
            val next = lines[from - 1].toByteArray(Charsets.UTF_8).size + 1L
            if (size + next > keepBytes) break
            size += next
            from--
        }
        val tmp = File(dir, ".plays.jsonl.tmp")
        tmp.bufferedWriter(Charsets.UTF_8).use { w -> for (i in from until lines.size) { w.write(lines[i]); w.newLine() } }
        if (!tmp.renameTo(plays)) { tmp.copyTo(plays, overwrite = true); tmp.delete() }
    }

    @Synchronized
    fun writeOpen(state: Map<String, Any?>?) {
        if (state == null) { open.delete(); return }
        dir.mkdirs()
        val tmp = File(dir, ".open.json.tmp")
        tmp.writeText(line(state) + "\n", Charsets.UTF_8)
        if (!tmp.renameTo(open)) { tmp.copyTo(open, overwrite = true); tmp.delete() }
    }

    @Synchronized
    fun readOpen(): Map<*, *>? = try {
        if (open.isFile) Json.parse(open.readText(Charsets.UTF_8).trim()) as? Map<*, *> else null
    } catch (_: Exception) {
        null
    }
}
