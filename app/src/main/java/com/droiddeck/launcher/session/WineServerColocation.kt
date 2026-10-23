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
 * Puts a Windows game's busiest thread and its wineserver on one small core, when that makes the game faster.
 *
 * Some games make thousands of wineserver requests a second from one thread: Escape from Duckov's
 * main thread creates and closes an anonymous semaphore ~4000 times a second (Proton runs esync here:
 * futex_waitv is blocked by Android's seccomp policy). Across cores each round trip costs two
 * wake-ups, and the scheduler keeps a thread that sleeps that often on the small cores anyway. On
 * one small core Duckov ran 56 fps instead of 47 on the Xring O3; one middle core was slower than
 * none, and the kernel widens a mask holding only prime cores.
 *
 * Whether it helps depends on the game and the scene, so fps is measured unpinned and pinned, and
 * the pin kept only when faster, and measured again every [RECHECK_NS] ns.
 */
object WineServerColocation {
    private const val TAG = "WineColocate"
    private const val WINDOW_NS = 5_000_000_000L
    private const val RECHECK_NS = 90_000_000_000L
    private const val COOLDOWN_NS = 120_000_000_000L
    private const val MIN_SHARE = 0.5
    private const val KEEP_SHARE = 0.3
    private const val MIN_SWITCHES_PER_S = 1500.0
    private const val GAIN = 1.04
    private const val MIN_FPS = 5.0
    private const val STALLS = 15
    private val WINE_SERVICES = setOf("services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "explorer.exe",
        "rpcss.exe", "steam.exe", "tabtip.exe", "conhost.exe", "start.exe", "wineboot.exe", "UnityCrashHandler64.exe",
        "UnityCrashHandler32.exe", "CrashReportClient.exe")

    @Volatile private var worker: Thread? = null

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

    /** The highest-numbered core of the lowest-capacity cluster: cpu0 takes most interrupts. */
    internal fun pickCore(): Int? {
        val cpus = (0 until Runtime.getRuntime().availableProcessors()).mapNotNull { cpu ->
            readLong("/sys/devices/system/cpu/cpu$cpu/cpu_capacity")?.let { cpu to it }
        }
        if (cpus.size < 2) return null
        val low = cpus.minOf { it.second }
        if (cpus.all { it.second == low }) return null
        return cpus.filter { it.second == low }.maxOf { it.first }
    }

    private class Candidate(val pid: Int, val tid: Int, val name: String)

    private enum class Phase { BASE, TRY, PINNED, COOLDOWN }

    private class Trial(val game: Candidate, val server: Int) {
        var phase = Phase.BASE
        var since = System.nanoTime()
        var frames0 = WaylandCompositor.gameFrameCount()
        var baseFps = 0.0
        var checked = 0L
        var stalls = 0
        var pinned = false

        fun restart(phase: Phase) {
            this.phase = phase
            since = System.nanoTime()
            frames0 = WaylandCompositor.gameFrameCount()
        }

        fun fps(): Double = (WaylandCompositor.gameFrameCount() - frames0) / ((System.nanoTime() - since) / 1e9)
    }

    private fun run(core: Int) {
        val scan = Scanner()
        var trial: Trial? = null
        var lastCount = WaylandCompositor.gameFrameCount()
        try {
            while (!Thread.currentThread().isInterrupted && worker === Thread.currentThread()) {
                TimeUnit.SECONDS.sleep(1)
                val found = scan.sample()
                val t = trial
                if (t != null && (!scan.alive(t.game.tid) || scan.share(t.game.tid) < KEEP_SHARE)) {
                    if (t.pinned) unpin(t, scan, core)
                    trial = null
                }
                if (trial == null) {
                    if (found != null) {
                        trial = Trial(found.first, found.second)
                        Log.i(TAG, "candidate ${found.first.name}/${found.first.tid}, wineserver ${found.second}: measuring")
                    }
                    lastCount = WaylandCompositor.gameFrameCount()
                    continue
                }
                val tr = trial ?: continue
                val count = WaylandCompositor.gameFrameCount()
                val stalled = count - lastCount < MIN_FPS
                lastCount = count
                if (tr.pinned) scan.loosenChildren(tr.game.pid, tr.game.tid, core)
                if (stalled && (tr.phase == Phase.BASE || tr.phase == Phase.TRY)) {
                    // A second without frames says nothing about the pin; a pin under which frames keep stopping is dropped.
                    if (tr.phase == Phase.TRY && ++tr.stalls > STALLS) {
                        unpin(tr, scan, core)
                        tr.restart(Phase.COOLDOWN)
                    } else {
                        tr.restart(tr.phase)
                    }
                    continue
                }
                val elapsed = System.nanoTime() - tr.since
                when (tr.phase) {
                    Phase.BASE -> if (elapsed >= WINDOW_NS) {
                        tr.baseFps = tr.fps()
                        when {
                            tr.baseFps < MIN_FPS -> tr.restart(Phase.COOLDOWN)
                            pin(tr, core) -> { tr.stalls = 0; tr.restart(Phase.TRY) }
                            else -> {
                                Log.i(TAG, "could not pin ${tr.game.name}/${tr.game.tid} to core $core")
                                tr.restart(Phase.COOLDOWN)
                            }
                        }
                    }
                    Phase.TRY -> if (elapsed >= WINDOW_NS) {
                        val fps = tr.fps()
                        val verdict = "${tr.game.name}/${tr.game.tid} with wineserver on core $core: " +
                            "${"%.1f".format(fps)} fps against ${"%.1f".format(tr.baseFps)} unpinned"
                        if (fps >= tr.baseFps * GAIN) {
                            Log.i(TAG, "$verdict, kept")
                            tr.phase = Phase.PINNED
                            tr.checked = System.nanoTime()
                        } else {
                            Log.i(TAG, "$verdict, undone")
                            unpin(tr, scan, core)
                            tr.restart(Phase.COOLDOWN)
                        }
                    }
                    // A busy core is the point of the pin (the thread waits less), so only fps decides: measured again now and then.
                    Phase.PINNED -> if (System.nanoTime() - tr.checked >= RECHECK_NS) {
                        unpin(tr, scan, core)
                        tr.restart(Phase.BASE)
                    }
                    Phase.COOLDOWN -> if (elapsed >= COOLDOWN_NS) tr.restart(Phase.BASE)
                }
            }
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped", t)
        } finally {
            trial?.let { if (it.pinned) unpin(it, scan, core) }
        }
    }

    private fun pin(t: Trial, core: Int): Boolean {
        t.pinned = true
        val ok = taskset(1L shl core, t.game.tid) && taskset(1L shl core, t.server, allThreads = true) &&
            runCatching {
                File("/proc/${t.game.tid}/status").readLines()
                    .any { it.startsWith("Cpus_allowed_list:") && it.substringAfter(':').trim() == "$core" }
            }.getOrDefault(false)
        if (!ok) unpin(t)
        return ok
    }

    private fun unpin(t: Trial, scan: Scanner? = null, core: Int = -1) {
        val all = (1L shl Runtime.getRuntime().availableProcessors()) - 1
        taskset(all, t.game.tid)
        taskset(all, t.server, allThreads = true)
        if (scan != null && core >= 0) scan.loosenChildren(t.game.pid, t.game.tid, core, recheck = true)
        t.pinned = false
    }

    /** toybox taskset: a hex mask; -a for every thread of the process. */
    private fun taskset(mask: Long, id: Int, allThreads: Boolean = false): Boolean {
        val args = listOf("/system/bin/taskset", if (allThreads) "-ap" else "-p", java.lang.Long.toHexString(mask), id.toString())
        var p: java.lang.Process? = null
        return try {
            p = ProcessBuilder(args).redirectErrorStream(true).start()
            p.inputStream.readBytes()
            p.waitFor(5, TimeUnit.SECONDS) && p.exitValue() == 0
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            false
        } catch (e: Exception) {
            Log.w(TAG, "taskset ${java.lang.Long.toHexString(mask)} $id", e)
            false
        } finally {
            p?.destroy()
        }
    }

    private fun readLong(path: String): Long? = try { File(path).readText().trim().toLong() } catch (_: Exception) { null }

    /**
     * Once a second, over this UID's processes (all /proc shows an app): the busiest thread of a
     * Windows game process with many voluntary switches (round trips), and its wineserver.
     */
    private class Scanner {
        private val self = Process.myPid()
        private val hz = Os.sysconf(OsConstants._SC_CLK_TCK).toDouble()
        private var lastNs = 0L
        private var lastTicks = HashMap<Int, Long>()
        private var lastSwitches = HashMap<Int, Long>()
        private var shares = HashMap<Int, Double>()
        private val known = HashSet<Int>()

        fun share(tid: Int): Double = shares[tid] ?: 0.0

        fun alive(tid: Int): Boolean = tid in shares

        /** Threads the pinned one started inherited its one-core mask: give them every core (only ours, not masks the game set). */
        fun loosenChildren(pid: Int, pinned: Int, core: Int, recheck: Boolean = false) {
            val all = (1L shl Runtime.getRuntime().availableProcessors()) - 1
            File("/proc/$pid/task").list()?.forEach { entry ->
                val tid = entry.toIntOrNull() ?: return@forEach
                if (tid == pinned || !known.add(tid) && !recheck) return@forEach
                val mask = runCatching {
                    File("/proc/$pid/task/$tid/status").readLines().firstOrNull { it.startsWith("Cpus_allowed:") }
                        ?.substringAfter(':')?.trim()?.toLong(16)
                }.getOrNull()
                if (mask == 1L shl core) {
                    ProcessBuilder("/system/bin/taskset", "-p", java.lang.Long.toHexString(all), tid.toString())
                        .redirectErrorStream(true).start().apply { inputStream.readBytes(); waitFor(5, TimeUnit.SECONDS); destroy() }
                }
            }
        }

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
                // Wine shows a Windows process by its Windows path: S:\...\Game.exe.
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
            // One wineserver per prefix: the game's has its WINEPREFIX.
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
