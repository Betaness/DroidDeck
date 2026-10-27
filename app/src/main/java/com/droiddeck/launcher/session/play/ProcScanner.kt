package com.droiddeck.launcher.session.play

import java.io.File

/**
 * [mali-g2] This app's processes as the play journal needs them: Steam game launches and the games' own
 * processes. Android shows an app only its own uid's processes in /proc (some hundred during a session),
 * and argv is read once per pid: a scan is a directory listing plus a few small reads for new pids.
 *
 * A process seen between fork and exec still shows its parent's argv, and wine renames its processes to
 * the Windows path some time after they start (seconds under FEX and proot): a young pid whose argv is
 * neither a launch nor an .exe is read again for [rereadScans] scans.
 */
class ProcScanner(private val proc: File = File("/proc"), private val rereadScans: Int = 6) {
    class Entry(
        val pid: Int,
        /** Start time in clock ticks after boot (/proc/<pid>/stat field 22). */
        val startTicks: Long,
        val argv0: String,
        val launch: LaunchCmdline.Launch?,
        internal val firstScan: Int = 0,
    ) {
        internal val settled: Boolean get() = launch != null || argv0.replace('\\', '/').substringAfterLast('/').lowercase().endsWith(".exe")
    }

    class LaunchGroup(val appid: Long, val pids: Set<Int>, val startTicks: Long, val exe: String?)

    private val known = HashMap<Int, Entry>()
    private var scans = 0

    fun scan(): Map<Int, Entry> {
        val names = proc.list() ?: return emptyMap()
        scans++
        val alive = HashSet<Int>(names.size)
        for (name in names) {
            val c = name.firstOrNull() ?: continue
            if (c !in '0'..'9') continue
            val pid = name.toIntOrNull() ?: continue
            alive.add(pid)
            val e = known[pid]
            if (e == null) read(pid, scans)?.let { known[pid] = it }
            else if (!e.settled && scans - e.firstScan <= rereadScans) read(pid, e.firstScan)?.let { known[pid] = it }
        }
        known.keys.retainAll(alive)
        return known
    }

    private fun read(pid: Int, firstScan: Int): Entry? {
        val argv = cmdline(pid) ?: return null
        // A kernel thread or a process between fork and exec: read it again next time.
        if (argv.isEmpty()) return null
        return Entry(pid, startTicks(pid) ?: 0L, argv[0], LaunchCmdline.parse(argv), firstScan)
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

    fun environ(pid: Int): List<String>? = try {
        String(File(proc, "$pid/environ").readBytes(), Charsets.UTF_8).split('\u0000').filter { it.isNotEmpty() }
    } catch (_: Exception) {
        null
    }

    companion object {
        fun launches(entries: Map<Int, Entry>): Map<Long, LaunchGroup> {
            val byApp = entries.values.filter { it.launch != null }.groupBy { it.launch!!.appid }
            return byApp.mapValues { (appid, list) ->
                LaunchGroup(appid, list.map { it.pid }.toSet(), list.minOf { it.startTicks },
                    list.firstNotNullOfOrNull { it.launch?.exe })
            }
        }

        private val WINE_SYSTEM = setOf(
            "services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "rpcss.exe", "explorer.exe", "tabtip.exe",
            "conhost.exe", "start.exe", "wineboot.exe", "steam.exe", "steamwebhelper.exe", "rundll32.exe", "winemenubuilder.exe",
            "cmd.exe", "crashpad_handler.exe", "unitycrashhandler64.exe", "easyanticheat.exe", "steamerrorreporter.exe",
        )

        fun windowsProcesses(entries: Map<Int, Entry>): List<Entry> = entries.values.filter { e ->
            val base = e.argv0.replace('\\', '/').substringAfterLast('/').lowercase()
            e.launch == null && base.endsWith(".exe") && base !in WINE_SYSTEM
        }

        fun gameProcess(entries: Map<Int, Entry>, exeName: String?): Entry? {
            if (exeName.isNullOrEmpty()) return null
            return entries.values.filter { it.launch == null && LaunchCmdline.runs(it.argv0, exeName) }
                .maxByOrNull { it.startTicks }
        }
    }
}
