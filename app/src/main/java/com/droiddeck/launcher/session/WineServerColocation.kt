package com.droiddeck.launcher.session

import android.content.Context
import android.os.Process
import android.system.Os
import android.system.OsConstants
import android.util.Log
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Puts a Windows game's busiest thread and its wineserver on one CPU core, when that makes the game faster.
 *
 * Some games make thousands of wineserver requests a second from their main thread: Escape from
 * Duckov (Unity) creates and closes an anonymous semaphore about 4000 times a second, each a round
 * trip to wineserver. With the two on different cores every request costs two cross-core wake-ups
 * (about 27 us of sleeping and waiting to run again), and the scheduler, seeing a thread that sleeps
 * 8000 times a second, keeps it on the small cores anyway. On one core they hand over without
 * either: Duckov went from 47 to 56 fps on the Xring O3, on any single small core; a single
 * middle core was slower than none, and the kernel will not let a thread onto the prime cores alone.
 *
 * Whether it helps depends on the game and the scene, so it is tried rather than assumed: the
 * frame rate is measured unpinned, then pinned, and the pin kept only when it is faster; it is
 * measured again every [RECHECK_S] seconds, and undone at once if the thread saturates the core.
 * Frames come from the compositor (WaylandCompositor.gameFrameCount), so a game without frames is
 * left alone. Affinity is set with toybox taskset: the guest's processes run under this app's UID.
 */
object WineServerColocation {
    private const val TAG = "WineColocate"
    private const val WINDOW_S = 5
    private const val RECHECK_S = 90
    private const val COOLDOWN_S = 120
    private const val MIN_SHARE = 0.5
    private const val KEEP_SHARE = 0.3
    private const val SATURATED = 0.97
    private const val MIN_SWITCHES_PER_S = 1500.0
    private const val GAIN = 1.04
    private const val MIN_FPS = 5.0
    /** Wine's own processes, which are never the game. */
    private val WINE_SERVICES = setOf("services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "explorer.exe",
        "rpcss.exe", "steam.exe", "tabtip.exe", "conhost.exe", "start.exe", "wineboot.exe", "UnityCrashHandler64.exe",
        "UnityCrashHandler32.exe", "CrashReportClient.exe")

    private var worker: Thread? = null

    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.wineColocate(context)) return "wine colocation: off"
        val core = pickCore() ?: return "wine colocation: no core to use"
        worker?.interrupt()
        worker = Thread({ run(core) }, "wine-colocate").apply { isDaemon = true; start() }
        return "wine colocation: watching (core $core)"
    }

    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker = null
    }

    /**
     * The core to share: the fastest-clocked core outside the highest-capacity cluster (the kernel
     * widens a mask holding only prime cores), the highest-numbered of equals, away from cpu0's interrupts.
     */
    internal fun pickCore(): Int? {
        val cpus = (0 until Runtime.getRuntime().availableProcessors()).mapNotNull { cpu ->
            val capacity = readLong("/sys/devices/system/cpu/cpu$cpu/cpu_capacity")
            val maxFreq = readLong("/sys/devices/system/cpu/cpu$cpu/cpufreq/scaling_max_freq")
            if (capacity == null || maxFreq == null) null else Triple(cpu, capacity, maxFreq)
        }
        if (cpus.size < 2) return null
        val top = cpus.maxOf { it.second }
        return cpus.filter { it.second < top }
            .maxWithOrNull(compareBy<Triple<Int, Long, Long>> { it.third }.thenBy { it.first })?.first
    }

    private class Candidate(val pid: Int, val tid: Int, val name: String)

    private enum class Phase { IDLE, BASE, TRY, PINNED, COOLDOWN }

    private fun run(core: Int) {
        val scan = Scanner()
        var candidate: Candidate? = null
        var server = -1
        var phase = Phase.IDLE
        var phaseStart = 0L
        var frames0 = 0L
        var baseFps = 0.0
        var lastCheck = 0L
        try {
            while (!Thread.currentThread().isInterrupted) {
                TimeUnit.SECONDS.sleep(1)
                val now = System.nanoTime() / 1_000_000_000
                val found = scan.sample()
                val current = candidate
                // Kept while it exists and stays busy: loading spikes on other threads do not take over.
                if (current != null && (!scan.alive(current.tid) || scan.share(current.tid) < KEEP_SHARE)) {
                    // The thread is gone or another took over: let wineserver run anywhere again.
                    if (server > 0 && phase in setOf(Phase.TRY, Phase.PINNED)) unpin(server)
                    candidate = null
                    phase = Phase.IDLE
                }
                if (candidate == null) {
                    if (found == null) continue
                    candidate = found.first
                    server = found.second
                    phase = Phase.BASE
                    phaseStart = now
                    frames0 = WaylandCompositor.gameFrameCount()
                    Log.i(TAG, "candidate ${found.first.name}/${found.first.tid}, wineserver $server: measuring")
                    continue
                }
                val c = candidate ?: continue
                val share = scan.share(c.tid)
                when (phase) {
                    Phase.BASE -> if (now - phaseStart >= WINDOW_S) {
                        baseFps = (WaylandCompositor.gameFrameCount() - frames0).toDouble() / (now - phaseStart)
                        if (baseFps < MIN_FPS) {
                            phase = Phase.COOLDOWN; phaseStart = now
                        } else if (pin(c.tid, server, core)) {
                            phase = Phase.TRY; phaseStart = now; frames0 = WaylandCompositor.gameFrameCount()
                        } else {
                            Log.i(TAG, "could not pin ${c.name}/${c.tid} to core $core")
                            phase = Phase.COOLDOWN; phaseStart = now
                        }
                    }
                    Phase.TRY -> if (now - phaseStart >= WINDOW_S) {
                        val fps = (WaylandCompositor.gameFrameCount() - frames0).toDouble() / (now - phaseStart)
                        if (fps >= baseFps * GAIN && share < SATURATED) {
                            Log.i(TAG, "${c.name}/${c.tid} and wineserver on core $core: %.1f fps against %.1f, kept".format(fps, baseFps))
                            phase = Phase.PINNED; lastCheck = now
                        } else {
                            Log.i(TAG, "${c.name}/${c.tid} on core $core: %.1f fps against %.1f, undone".format(fps, baseFps))
                            unpin(c.tid); unpin(server)
                            phase = Phase.COOLDOWN; phaseStart = now
                        }
                    }
                    Phase.PINNED -> when {
                        share >= SATURATED -> {
                            Log.i(TAG, "${c.name}/${c.tid} saturates core $core, undone")
                            unpin(c.tid); unpin(server)
                            phase = Phase.COOLDOWN; phaseStart = now
                        }
                        now - lastCheck >= RECHECK_S -> {
                            // Measure the game unpinned again; the scene may have changed.
                            unpin(c.tid); unpin(server)
                            phase = Phase.BASE; phaseStart = now; frames0 = WaylandCompositor.gameFrameCount()
                        }
                    }
                    Phase.COOLDOWN -> if (now - phaseStart >= COOLDOWN_S) {
                        phase = Phase.BASE; phaseStart = now; frames0 = WaylandCompositor.gameFrameCount()
                    }
                    Phase.IDLE -> {}
                }
            }
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped", t)
        } finally {
            candidate?.let { if (phase == Phase.TRY || phase == Phase.PINNED) { unpin(it.tid); unpin(server) } }
        }
    }

    private fun pin(tid: Int, server: Int, core: Int): Boolean =
        taskset("$core", tid) && taskset("$core", server, allThreads = true) &&
            File("/proc/$tid/status").readLines().any { it.startsWith("Cpus_allowed_list:") && it.substringAfter(':').trim() == "$core" }

    private fun unpin(tid: Int) {
        taskset("0-${Runtime.getRuntime().availableProcessors() - 1}", tid, allThreads = true)
    }

    private fun taskset(cpus: String, id: Int, allThreads: Boolean = false): Boolean = try {
        val args = mutableListOf("/system/bin/taskset")
        if (allThreads) args += "-a"
        args += listOf("-p", "-c", cpus, id.toString())
        val p = ProcessBuilder(args).redirectErrorStream(true).start()
        p.inputStream.readBytes()
        p.waitFor(5, TimeUnit.SECONDS) && p.exitValue() == 0
    } catch (e: Exception) {
        Log.w(TAG, "taskset $cpus $id", e)
        false
    }

    private fun readLong(path: String): Long? = try { File(path).readText().trim().toLong() } catch (_: Exception) { null }

    /**
     * Once a second: the busiest thread of a Windows game process (by CPU share and voluntary context
     * switches, the sign of round trips) and its wineserver's pid. /proc shows this app's UID only.
     */
    private class Scanner {
        private val self = Process.myPid()
        private val hz = Os.sysconf(OsConstants._SC_CLK_TCK).toDouble()
        private var lastNs = 0L
        private var lastTicks = HashMap<Int, Long>()
        private var lastSwitches = HashMap<Int, Long>()
        private var shares = HashMap<Int, Double>()

        fun share(tid: Int): Double = shares[tid] ?: 0.0

        fun alive(tid: Int): Boolean = tid in shares

        fun sample(): Pair<Candidate, Int>? {
            val now = System.nanoTime()
            val seconds = if (lastNs == 0L) 0.0 else (now - lastNs) / 1e9
            lastNs = now
            val servers = ArrayList<Int>()
            val games = ArrayList<Int>()
            File("/proc").list()?.forEach { entry ->
                val pid = entry.toIntOrNull() ?: return@forEach
                if (pid == self) return@forEach
                val comm = read("/proc/$pid/comm") ?: return@forEach
                if (comm == "wineserver") { servers += pid; return@forEach }
                val cmd = read("/proc/$pid/cmdline")?.substringBefore('\u0000') ?: return@forEach
                // Wine shows Windows processes by their Windows path: S:\...\Game.exe.
                if (cmd.length > 3 && cmd[1] == ':' && cmd[2] == '\\' && cmd.endsWith(".exe", ignoreCase = true) &&
                    cmd.substringAfterLast('\\') !in WINE_SERVICES) games += pid
            }
            val ticks = HashMap<Int, Long>()
            val switches = HashMap<Int, Long>()
            val newShares = HashMap<Int, Double>()
            var best: Candidate? = null
            var bestShare = 0.0
            for (pid in games) {
                File("/proc/$pid/task").list()?.forEach { entry ->
                    val tid = entry.toIntOrNull() ?: return@forEach
                    val t = cpuTicks("/proc/$pid/task/$tid/stat") ?: return@forEach
                    val s = voluntarySwitches("/proc/$pid/task/$tid/status") ?: return@forEach
                    ticks[tid] = t
                    switches[tid] = s
                    if (seconds <= 0) return@forEach
                    val share = (t - (lastTicks[tid] ?: t)) / hz / seconds
                    val rate = (s - (lastSwitches[tid] ?: s)) / seconds
                    newShares[tid] = share
                    if (share >= MIN_SHARE && rate >= MIN_SWITCHES_PER_S && share > bestShare) {
                        bestShare = share
                        best = Candidate(pid, tid, read("/proc/$pid/task/$tid/comm") ?: "?")
                    }
                }
            }
            lastTicks = ticks
            lastSwitches = switches
            shares = newShares
            val b = best ?: return null
            // One wineserver per prefix: the game's is the one with its WINEPREFIX.
            val server = servers.singleOrNull() ?: prefix(b.pid)?.let { p -> servers.firstOrNull { prefix(it) == p } }
            return server?.let { b to it }
        }

        private fun prefix(pid: Int): String? = try {
            File("/proc/$pid/environ").readText().split('\u0000').firstOrNull { it.startsWith("WINEPREFIX=") }
        } catch (_: Exception) {
            null
        }

        private fun read(path: String): String? = try { File(path).readText().trim() } catch (_: Exception) { null }

        private fun cpuTicks(path: String): Long? = try {
            val line = File(path).readText()
            val fields = line.substring(line.lastIndexOf(')') + 2).split(' ')
            fields[11].toLong() + fields[12].toLong()
        } catch (_: Exception) {
            null
        }

        private fun voluntarySwitches(path: String): Long? = try {
            File(path).readLines().firstOrNull { it.startsWith("voluntary_ctxt_switches:") }?.substringAfter(':')?.trim()?.toLong()
        } catch (_: Exception) {
            null
        }
    }
}
