package com.droiddeck.launcher.session.play

import java.io.File

/**
 * [mali-g2] This app's processes as the play journal needs them: Steam game launches and the games' own
 * processes. Android shows an app only its own uid's processes in /proc (some hundred during a session),
 * and argv is read once per pid: a scan is a directory listing plus a few small reads for new pids.
 */
class ProcScanner(private val proc: File = File("/proc")) {
    class Entry(
        val pid: Int,
        /** Start time in clock ticks after boot (/proc/<pid>/stat field 22). */
        val startTicks: Long,
        val argv0: String,
        val launch: LaunchCmdline.Launch?,
    )

    class LaunchGroup(val appid: Long, val pids: Set<Int>, val startTicks: Long, val exe: String?)

    private val known = HashMap<Int, Entry>()

    /** The processes alive now. Pids gone since the last scan are forgotten. */
    fun scan(): Map<Int, Entry> {
        val names = proc.list() ?: return emptyMap()
        val alive = HashSet<Int>(names.size)
        for (name in names) {
            val c = name.firstOrNull() ?: continue
            if (c !in '0'..'9') continue
            val pid = name.toIntOrNull() ?: continue
            alive.add(pid)
            if (pid !in known) read(pid)?.let { known[pid] = it }
        }
        known.keys.retainAll(alive)
        return known
    }

    private fun read(pid: Int): Entry? {
        val argv = cmdline(pid) ?: return null
        // A kernel thread or a process between fork and exec: read it again next time.
        if (argv.isEmpty()) return null
        return Entry(pid, startTicks(pid) ?: 0L, argv[0], LaunchCmdline.parse(argv))
    }

    fun cmdline(pid: Int): List<String>? = try {
        val bytes = File(proc, "$pid/cmdline").readBytes()
        if (bytes.isEmpty()) emptyList() else String(bytes, Charsets.UTF_8).trimEnd('\u0000').split('\u0000')
    } catch (_: Exception) {
        null
    }

    fun startTicks(pid: Int): Long? = try {
        val stat = File(proc, "$pid/stat").readText()
        // Field 2 (comm) may hold spaces and parentheses: count from the last ')'.
        val fields = stat.substring(stat.lastIndexOf(')') + 2).split(' ')
        fields.getOrNull(19)?.toLongOrNull()
    } catch (_: Exception) {
        null
    }

    /** /proc/<pid>/status values in kB, e.g. VmHWM, VmRSS, RssAnon. */
    fun statusKb(pid: Int, vararg keys: String): Map<String, Long> {
        val out = HashMap<String, Long>()
        try {
            File(proc, "$pid/status").forEachLine { line ->
                val colon = line.indexOf(':')
                if (colon <= 0) return@forEachLine
                val key = line.substring(0, colon)
                if (key in keys) line.substring(colon + 1).trim().substringBefore(' ').toLongOrNull()?.let { out[key] = it }
            }
        } catch (_: Exception) {
        }
        return out
    }

    /** /proc/<pid>/environ as KEY=VALUE strings; null when it cannot be read. */
    fun environ(pid: Int): List<String>? = try {
        String(File(proc, "$pid/environ").readBytes(), Charsets.UTF_8).split('\u0000').filter { it.isNotEmpty() }
    } catch (_: Exception) {
        null
    }

    companion object {
        /** Launches by app id: the pids carrying it (wrapper and reaper), the earliest start and the exe. */
        fun launches(entries: Map<Int, Entry>): Map<Long, LaunchGroup> {
            val byApp = entries.values.filter { it.launch != null }.groupBy { it.launch!!.appid }
            return byApp.mapValues { (appid, list) ->
                LaunchGroup(appid, list.map { it.pid }.toSet(), list.minOf { it.startTicks },
                    list.firstNotNullOfOrNull { it.launch?.exe })
            }
        }

        /** The game's own process: argv[0] names [exeName]; the newest when several (a launcher that starts the game). */
        fun gameProcess(entries: Map<Int, Entry>, exeName: String?): Entry? {
            if (exeName.isNullOrEmpty()) return null
            return entries.values.filter { it.launch == null && LaunchCmdline.runs(it.argv0, exeName) }
                .maxByOrNull { it.startTicks }
        }
    }
}
