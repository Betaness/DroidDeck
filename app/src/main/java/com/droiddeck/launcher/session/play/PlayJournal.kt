package com.droiddeck.launcher.session.play

import android.app.ActivityManager
import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Display
import com.droiddeck.launcher.BuildConfig
import com.droiddeck.launcher.core.DeviceReport
import com.droiddeck.launcher.core.LogRedactor
import com.droiddeck.launcher.runtime.LinuxRuntime
import com.droiddeck.launcher.session.BatteryComponent
import com.droiddeck.launcher.session.FrameSchedOptOut
import com.droiddeck.launcher.session.HyperOsGameMode
import com.droiddeck.launcher.session.MiBridgeBoost
import com.droiddeck.launcher.session.SessionPrefs
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.session.VortekServerComponent
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

/**
 * [mali-g2] The play journal: one compact record per game played, in getExternalFilesDir()/journal/plays.jsonl,
 * for reading over adb after the owner has played (mali-g2/tools/play-journal.py prints it). What ran, how
 * well (fps, frame times, refresh rate, temperatures, clocks, memory), how it ended, and which session folder
 * holds the full logs; the folder is kept longer when the play had a problem ([KeepMarker], MirrorRetention).
 *
 * Cost while playing: the compositor bumps a histogram per game frame ([FrameTimes]); every 5 s a background
 * thread lists this app's processes (argv read once per new pid), reads three cpufreq files and some
 * volatile fields; every 30 s it reads two /proc status files, the memory info and the thermal headroom.
 * The logs are read only after a play has ended ([LogSliceScanner]).
 *
 * A play is one Steam launch (the wrapper's argv carries `SteamLaunch AppId=`), or the whole session for a
 * game run directly (MODE_RUN). A play still open when the app dies is in open.json (rewritten every
 * minute) and is recorded at the next session start ([recover]).
 */
object PlayJournal {
    private const val TAG = "PlayJournal"
    private const val TICK_MS = 5_000L
    private const val SLOW_MS = 30_000L
    private const val CHECKPOINT_MS = 60_000L
    private const val ENV_AFTER_MS = 20_000L
    private const val RELAUNCH_MS = 15_000L
    private const val SHELL_FRESH_MS = 90_000L
    private const val MARK_KEEP_MS = 30L * 24 * 3600 * 1000
    private const val STEAM_UI = 769L
    private const val RUN_KEY = -1L

    private val facts = ConcurrentHashMap<String, String>()
    private val lock = ReentrantLock()
    @Volatile private var handler: Handler? = null

    private var app: Context? = null
    private var dir: File? = null
    private var mode = ""
    private var program: String? = null
    private var journal: JournalFile? = null
    private var scanner = ProcScanner()
    private val open = LinkedHashMap<Long, PlayState>()
    private val counting = HashMap<Long, Boolean>()
    private val windows = HashMap<Long, MutableSet<String>>()
    private var playCount = 0
    private var lastEnded: Triple<Long, String, Long>? = null // appid, id, end
    private var focusedApp: Long? = null
    private val focusableSeen = HashSet<Long>()
    private var lastHist = FrameTimes.snapshot()
    private var lastSlowAt = 0L
    private var lastCheckpointAt = 0L
    private val recent = ArrayDeque<LongArray>() // [t, frames]
    @Volatile private var thermalStatus: Int? = null
    private var thermalListener: Any? = null
    private var headroom: Float? = null
    private var running = false
    private var policies: List<File> = emptyList()
    private var logFiles: List<Pair<String, File>> = emptyList()
    /** The logs' lengths at the tick before: a launch is seen up to a tick late, and its first lines matter. */
    private var lengthsBefore: Map<String, Long> = emptyMap()
    private val clkTck: Long by lazy {
        runCatching { android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK) }.getOrDefault(100L).takeIf { it > 0 } ?: 100L
    }

    /** Wraps a line the session logs at its start ("mibridge: ...") so the plays record it too. */
    fun fact(key: String, line: String): String {
        facts[key] = line
        return line
    }

    private fun thread(): Handler {
        handler?.let { return it }
        synchronized(this) {
            handler?.let { return it }
            val t = HandlerThread("play-journal", Process.THREAD_PRIORITY_BACKGROUND).apply { start() }
            return Handler(t.looper).also { handler = it }
        }
    }

    private fun post(block: () -> Unit) {
        thread().post {
            lock.lock()
            try { block() } catch (t: Throwable) { Log.w(TAG, "journal", t) } finally { lock.unlock() }
        }
    }

    private fun journalFor(context: Context): JournalFile? =
        context.getExternalFilesDir(null)?.let { JournalFile(File(it, "journal"), LogRedactor::redact) }

    fun sessionStarted(context: Context, sessionDir: File, sessionMode: String, sessionProgram: String?) {
        facts.clear()
        val ctx = context.applicationContext
        post {
            app = ctx
            dir = sessionDir
            mode = sessionMode
            program = sessionProgram
            journal = journalFor(ctx)
            scanner = ProcScanner()
            open.clear()
            counting.clear()
            windows.clear()
            playCount = 0
            focusedApp = null
            focusableSeen.clear()
            recent.clear()
            lastHist = FrameTimes.snapshot()
            lastSlowAt = 0L
            lastCheckpointAt = System.currentTimeMillis()
            headroom = null
            running = true
            ShellProbe.reset()
            policies = File("/sys/devices/system/cpu/cpufreq").listFiles { f -> f.name.startsWith("policy") }
                ?.sortedBy { it.name.removePrefix("policy").toIntOrNull() ?: 99 }.orEmpty()
            listenThermal(ctx)
            logFiles = listOf(
                "session.log" to File(sessionDir, "session.log"),
                "app.log" to File(sessionDir, "app.log"),
                "vortek.log" to File(sessionDir, "vortek.log"),
                "gameprocess_log.txt" to File(SessionPrefs.activeSteamRoot(ctx), "logs/gameprocess_log.txt"),
            )
            lengthsBefore = logFiles.associate { (label, f) -> label to f.length() }
            if (sessionMode == SessionService.MODE_RUN) openRun(System.currentTimeMillis())
            thread().removeCallbacks(tickRunnable)
            thread().postDelayed(tickRunnable, TICK_MS)
        }
    }

    private val tickRunnable: Runnable = object : Runnable {
        override fun run() {
            lock.lock()
            try { tick() } catch (t: Throwable) { Log.w(TAG, "tick", t) } finally { lock.unlock() }
            if (running) handler?.postDelayed(this, TICK_MS)
        }
    }

    fun focus(focused: Long?, focusable: List<Long>) = post {
        if (!running) return@post
        val now = System.currentTimeMillis()
        if (open.isNotEmpty()) sample(now, slow = false)
        focusedApp = focused
        focusableSeen.addAll(focusable)
        focused?.let { focusableSeen.add(it) }
        for ((k, s) in open) counting[k] = focused(s) && SessionState.visible && !SessionState.suspended
    }

    fun vortekExited(status: Int) = post {
        val now = System.currentTimeMillis()
        for (s in open.values) if (s.vortekExit == null) { s.vortekExit = status; s.vortekExitAtMs = now }
    }

    /** At the start of stopSession, while the guest still runs. */
    fun sessionStopping(status: Int, failureCode: String?) = post {
        if (!running) return@post
        val now = System.currentTimeMillis()
        if (open.isNotEmpty()) sample(now, slow = false)
        val alive = if (mode == SessionService.MODE_RUN) emptySet() else ProcScanner.launches(scanner.scan()).keys
        for (k in open.keys.toList()) {
            val s = open[k] ?: continue
            when {
                k == RUN_KEY -> finish(s, now, End(runStatus = status, failureCode = failureCode))
                k in alive -> finish(s, now, End(stopped = true, sessionStatus = status, failureCode = failureCode))
                else -> finish(s, now, End())
            }
        }
        running = false
        handler?.removeCallbacks(tickRunnable)
        unlistenThermal()
        ShellProbe.wanted = false
        journal?.writeOpen(null)
    }

    /** On the session-collect thread, before the folder is collected: lets the plays' records and .keep land first. */
    fun finishSession(timeoutMs: Long = 20_000) {
        val h = handler ?: return
        val done = CountDownLatch(1)
        h.post { done.countDown() }
        try { done.await(timeoutMs, TimeUnit.MILLISECONDS) } catch (_: InterruptedException) {}
    }

    fun flushOpen() {
        if (!lock.tryLock(2, TimeUnit.SECONDS)) return
        try { checkpoint(System.currentTimeMillis()) } catch (_: Throwable) {} finally { lock.unlock() }
    }

    /**
     * The owner noted that something went wrong (session drawer). [drawerOpenedAt] is when the drawer was
     * opened: the problem was before that. The session's logs are kept for 30 days.
     */
    fun mark(drawerOpenedAt: Long): Long {
        val now = System.currentTimeMillis()
        post {
            val d = dir ?: return@post
            val s = open.values.lastOrNull()
            val m = linkedMapOf<String, Any?>(
                "v" to PlayRecord.VERSION,
                "type" to "mark",
                "t" to PlayRecord.iso(now),
                "drawerOpenedAt" to drawerOpenedAt.takeIf { it > 0 }?.let { PlayRecord.iso(it) },
                "logs" to d.name,
                "play" to s?.id,
                "appid" to s?.appid,
                "game" to s?.game,
                "playS" to s?.let { (now - it.startMs) / 1000 },
                "fpsLast60s" to fpsLast60s(),
                "modeHz" to modeHz(),
                "thermalStatus" to (thermalStatus ?: currentThermalStatus())?.let(PlayRecord::thermalName),
                "skinC" to ShellProbe.last?.takeIf { now - it.atMs < SHELL_FRESH_MS }?.skinC,
                "batteryC" to batteryC(),
            )
            journal?.append(m)
            for (p in open.values) p.marks += (now - p.startMs) / 1000 to now
            KeepMarker.extend(d, now + MARK_KEEP_MS, listOf("marked"))
            checkpoint(now)
            Log.i(TAG, "marked: something went wrong at ${PlayRecord.iso(now)}" + (s?.let { " in ${it.game}" } ?: "") +
                "; this session's logs are kept for 30 days")
        }
        return now
    }

    private fun tick() {
        if (!running) return
        val now = System.currentTimeMillis()
        track(now)
        if (open.isEmpty()) {
            lastHist = FrameTimes.snapshot()
            ShellProbe.wanted = false
            return
        }
        ShellProbe.wanted = true
        val slow = now - lastSlowAt >= SLOW_MS
        sample(now, slow)
        if (slow) lastSlowAt = now
        if (now - lastCheckpointAt >= CHECKPOINT_MS) checkpoint(now)
    }

    private fun track(now: Long) {
        if (mode == SessionService.MODE_RUN) {
            open[RUN_KEY]?.let { s -> resolveGame(s, scanner.scan(), now) }
            return
        }
        val entries = scanner.scan()
        val launches = ProcScanner.launches(entries)
        val gone = open.filterKeys { it !in launches }.values.toList()
        if (gone.isNotEmpty()) sample(now, slow = false)
        for (s in gone) finish(s, now, End())
        for ((appid, s) in open) {
            val g = launches[appid] ?: continue
            s.pids = g.pids
            resolveGame(s, entries, now)
        }
        for ((appid, g) in launches) if (appid !in open) openSteam(g, entries, now)
        lengthsBefore = logFiles.associate { (label, f) -> label to f.length() }
    }

    private fun resolveGame(s: PlayState, entries: Map<Int, ProcScanner.Entry>, now: Long) {
        if (s.gamePid <= 0 || s.gamePid !in entries || (s.gameGuessed && now - s.guessedAt >= SLOW_MS)) {
            val named = ProcScanner.gameProcess(entries, s.exe?.substringAfterLast('/'))
            // A launcher exe (start_protected_game.exe) that starts the real game: the busiest other Windows process.
            val e = named ?: if (now - s.startMs >= ENV_AFTER_MS) {
                ProcScanner.windowsProcesses(entries).maxByOrNull { scanner.statusKb(it.pid, "VmRSS")["VmRSS"] ?: 0L }
            } else null
            if (e != null) {
                if (e.pid != s.gamePid) s.env = null
                s.gamePid = e.pid
                s.gameProcess = e.argv0.replace('\\', '/').substringAfterLast('/')
                s.gameGuessed = named == null
                s.guessedAt = now
            }
        }
        if (s.env == null && s.gamePid > 0 && now - s.startMs >= ENV_AFTER_MS) {
            val environ = scanner.environ(s.gamePid)
            s.env = if (environ != null) EnvAllowlist.filter(environ) else mapOf("note" to "environ not readable")
        }
    }

    private fun openSteam(g: ProcScanner.LaunchGroup, entries: Map<Int, ProcScanner.Entry>, now: Long) {
        val ctx = app ?: return
        val d = dir ?: return
        val bootMs = System.currentTimeMillis() - SystemClock.elapsedRealtime()
        val start = (bootMs + g.startTicks * 1000 / clkTck).takeIf { g.startTicks > 0 && it in (now - 3_600_000)..now } ?: now
        val relaunch = lastEnded?.takeIf { it.first == g.appid && start - it.third <= RELAUNCH_MS }?.second
        val name = gameName(ctx, g.appid) ?: g.exe?.substringBefore('/')
        val s = PlayState("${d.name}#${++playCount}", d.name, g.appid, name, g.exe, "steam", start, relaunch)
        s.pids = g.pids
        prepare(ctx, s)
        open[g.appid] = s
        counting[g.appid] = false
        FrameTimes.forgetGap()
        resolveGame(s, entries, now)
        Log.i(TAG, "play ${s.id}: ${g.appid} $name started")
    }

    private fun openRun(now: Long) {
        val ctx = app ?: return
        val d = dir ?: return
        val prog = program
        val name = com.droiddeck.launcher.frontend.Library.nameForProgram(prog)
            ?: com.droiddeck.launcher.session.NoSteamLaunch.nameFor(prog, SessionState.programArgs)
            ?: prog?.substringAfterLast('/')
        val exe = (SessionState.programArgs.lastOrNull { it.endsWith(".exe", ignoreCase = true) } ?: prog)?.let(LaunchCmdline::relative)
        val s = PlayState("${d.name}#${++playCount}", d.name, null, name, exe, "run", now)
        prepare(ctx, s)
        open[RUN_KEY] = s
        counting[RUN_KEY] = true
        FrameTimes.forgetGap()
        Log.i(TAG, "play ${s.id}: $name started")
    }

    private fun prepare(ctx: Context, s: PlayState) {
        s.app = linkedMapOf(
            "version" to "${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})",
            "commit" to BuildConfig.SOURCE_COMMIT.take(12),
            "ci" to BuildConfig.CI_BUILD,
        )
        s.gpuAsked = runCatching { SessionPrefs.gpuPath(ctx) }.getOrNull()
        s.display = display(ctx)
        s.sources = logFiles.map { (label, f) -> LogSliceScanner.Source(label, f, lengthsBefore[label] ?: f.length()) }
    }

    private fun display(ctx: Context): Map<String, Any?> {
        val panel = displayNow(ctx)?.mode?.let { "${it.physicalWidth}x${it.physicalHeight}" }
        val size = SessionState.outputSize
        return linkedMapOf(
            "render" to "${size.first}x${size.second}",
            "choice" to ctx.getSharedPreferences("session", Context.MODE_PRIVATE)
                .getString("displayResolution.${SessionState.mode}", null),
            "panel" to panel,
            "swapchain" to null,
            "window" to if (SessionState.freeform) "freeform" else "fullscreen",
            "wantedHz" to SessionState.wantedHz.takeIf { it > 0f }?.let { Math.round(it) },
            "refreshHz" to Math.round(SessionState.refreshHz),
            "fpsLimit" to SessionState.fpsLimit,
            "upscaler" to runCatching { SessionPrefs.upscaler(ctx) }.getOrNull(),
            "hdr" to SessionState.hdr,
        )
    }

    private fun settings(ctx: Context?): Map<String, Any?> {
        val out = LinkedHashMap<String, Any?>()
        for (k in listOf("gpuclock", "threads", "schedfeed", "joyose", "mibridge", "colocation")) facts[k]?.let { out[k] = it }
        out["regulator"] = HyperOsGameMode.launchCheck.value.name
        out["joyoseWatch"] = HyperOsGameMode.joyoseWatch
        out["frameSchedOptOut"] = FrameSchedOptOut.outcome
        out["mibridgeSpec"] = ctx?.let { runCatching { SessionPrefs.miBridgeBoost(it) }.getOrNull() }
        return out
    }

    private fun focused(s: PlayState): Boolean {
        if (s.launch != "steam") return true
        val f = focusedApp ?: return true
        if (f == s.appid) return true
        // A launch gamescope never listed (some shortcuts): anything but Steam's own UI is the game.
        return s.appid !in focusableSeen && f != STEAM_UI
    }

    private fun sample(now: Long, slow: Boolean) {
        val frames = WaylandCompositor.gameFrameCount()
        val hist = FrameTimes.snapshot()
        val delta = FrameTimes.delta(hist, lastHist)
        lastHist = hist
        val visible = SessionState.visible
        val suspended = SessionState.suspended
        val ctx = app
        recent.addLast(longArrayOf(now, frames))
        while (recent.size > 13) recent.removeFirst()
        if (slow && ctx != null && Build.VERSION.SDK_INT >= 30) {
            headroom = runCatching { ctx.getSystemService(PowerManager::class.java)?.getThermalHeadroom(10) }.getOrNull()?.takeIf { !it.isNaN() }
        }
        val shell = ShellProbe.last?.takeIf { now - it.atMs < SHELL_FRESH_MS }
        val cpu = IntArray(policies.size)
        val cap = IntArray(policies.size)
        policies.forEachIndexed { i, p ->
            cpu[i] = readKhz(File(p, "scaling_cur_freq")) / 1000
            cap[i] = readKhz(File(p, "scaling_max_freq")) / 1000
        }
        val mode = displayNow(ctx)?.mode?.refreshRate
        val battery = batteryC()
        val status = thermalStatus ?: currentThermalStatus()
        val mi = MiBridgeBoost.lastResult.takeIf { it.isNotEmpty() }
        for ((k, s) in open) {
            if (counting[k] == true && visible && !suspended) FrameTimes.addInto(s.hist, delta)
            val focused = focused(s)
            s.acc.add(Sample(
                now, focused, visible, suspended, frames, mode, SessionState.wantedHz.takeIf { it > 0f }, shell?.limitHz,
                status, headroom, shell?.skinC, shell?.socC, battery,
                cpu.takeIf { policies.isNotEmpty() }, cap.takeIf { policies.isNotEmpty() }, mi,
            ))
            counting[k] = focused && visible && !suspended
            if (shell != null) s.shellSeen = true
            windows.getOrPut(k) { HashSet() }.add(if (SessionState.freeform) "freeform" else "fullscreen")
            if (slow) memory(s, ctx)
        }
    }

    private fun memory(s: PlayState, ctx: Context?) {
        val game = s.gamePid.takeIf { it > 0 }?.let { scanner.statusKb(it, "VmHWM", "VmRSS", "RssAnon") }
        val vortek = VortekServerComponent.runningPid.takeIf { it > 1 }?.let { scanner.statusKb(it, "VmHWM", "RssAnon") }
        val mi = ctx?.let { c ->
            runCatching { ActivityManager.MemoryInfo().also { c.getSystemService(ActivityManager::class.java)?.getMemoryInfo(it) } }.getOrNull()
        }
        s.acc.memory(
            gameHwmKb = game?.get("VmHWM"), gameRssKb = game?.get("VmRSS"), gameAnonKb = game?.get("RssAnon"),
            vortekAnonKb = vortek?.get("RssAnon"), vortekHwmKb = vortek?.get("VmHWM"),
            appNativeBytes = Debug.getNativeHeapAllocatedSize(), availBytes = mi?.availMem?.takeIf { it > 0 }, lowMemory = mi?.lowMemory,
        )
    }

    private fun readKhz(f: File): Int = try { f.readText().trim().toInt() } catch (_: Exception) { 0 }

    private fun displayNow(ctx: Context?): Display? = ctx?.let {
        runCatching {
            val dm = it.getSystemService(DisplayManager::class.java)
            dm?.getDisplay(SessionState.activityDisplayId) ?: dm?.getDisplay(Display.DEFAULT_DISPLAY)
        }.getOrNull()
    }

    private fun modeHz(): Int? = displayNow(app)?.mode?.refreshRate?.let { Math.round(it) }

    private fun batteryC(): Float? = BatteryComponent.lastTempDeci.takeIf { it != Int.MIN_VALUE && it != 0 }?.let { it / 10f }

    private fun currentThermalStatus(): Int? {
        val ctx = app ?: return null
        if (Build.VERSION.SDK_INT < 29) return null
        return runCatching { ctx.getSystemService(PowerManager::class.java)?.currentThermalStatus }.getOrNull()
    }

    private fun listenThermal(ctx: Context) {
        unlistenThermal()
        if (Build.VERSION.SDK_INT < 29) return
        val pm = ctx.getSystemService(PowerManager::class.java) ?: return
        val l = PowerManager.OnThermalStatusChangedListener { thermalStatus = it }
        runCatching { pm.addThermalStatusListener({ r -> handler?.post(r) }, l); thermalListener = l }
    }

    private fun unlistenThermal() {
        val l = thermalListener ?: return
        thermalListener = null
        if (Build.VERSION.SDK_INT < 29) return
        runCatching {
            app?.getSystemService(PowerManager::class.java)?.removeThermalStatusListener(l as PowerManager.OnThermalStatusChangedListener)
        }
    }

    private fun fpsLast60s(): List<Double?> {
        val out = ArrayList<Double?>()
        val list = recent.toList()
        for (i in 1 until list.size) {
            val dt = list[i][0] - list[i - 1][0]
            out.add(if (dt > 0) PlayAccumulator.r1((list[i][1] - list[i - 1][1]) * 1000.0 / dt) else null)
        }
        return out
    }

    private fun gameName(ctx: Context, appid: Long): String? {
        val name = Regex("^\\s*\"name\"\\s*\"([^\"]*)\"", RegexOption.MULTILINE)
        val libraries = runCatching { com.droiddeck.launcher.frontend.Library.steamLibraries(ctx).map { it.first } }.getOrNull()
            ?: listOf(SessionPrefs.activeSteamRoot(ctx))
        for (lib in libraries) {
            val acf = File(lib, "steamapps/appmanifest_$appid.acf")
            if (!acf.isFile) continue
            runCatching { name.find(acf.readText())?.groupValues?.get(1) }.getOrNull()?.takeIf { it.isNotBlank() }?.let { return it }
        }
        return null
    }

    private class End(
        val stopped: Boolean = false,
        val sessionStatus: Int? = null,
        val failureCode: String? = null,
        val runStatus: Int? = null,
    )

    private fun gameSources(ctx: Context, s: PlayState, f: LogSliceScanner.Findings, scanner: LogSliceScanner): List<LogSliceScanner.Source> {
        val out = ArrayList<LogSliceScanner.Source>()
        val since = s.startMs - 5_000
        val appid = s.appid ?: return out
        val pfx = runCatching { com.droiddeck.launcher.frontend.Library.protonPrefix(ctx, appid) }.getOrNull()
        if (pfx != null) {
            val user = File(pfx, "drive_c/users/steamuser")
            File(user, "AppData/LocalLow").listFiles()?.forEach { company ->
                company.listFiles()?.forEach { product ->
                    for (n in listOf("Player.log", "Player-prev.log")) {
                        val log = File(product, n)
                        if (log.isFile && log.lastModified() >= since) out += LogSliceScanner.Source("${product.name}/$n", log)
                    }
                }
            }
            File(user, "AppData/Local/Temp").listFiles()?.forEach { company ->
                company.listFiles()?.forEach { product ->
                    File(product, "Crashes").listFiles()?.filter { it.isDirectory && it.lastModified() >= since }?.take(3)?.forEach { crash ->
                        scanner.note(f, "crash", "Unity crash report ${crash.name}", "${product.name}/Crashes")
                        File(crash, "error.log").takeIf { it.isFile }?.let { out += LogSliceScanner.Source("${crash.name}/error.log", it) }
                    }
                }
            }
        }
        File(LinuxRuntime.rootDir(ctx), "root/steam-$appid.log").takeIf { it.isFile && it.lastModified() >= since }
            ?.let { out += LogSliceScanner.Source(it.name, it) }
        return out
    }

    /** This app's crash buffer since the play started (vortek-server is this app's child, so its crashes are there). */
    private fun crashBuffer(startMs: Long): File? = try {
        val since = java.text.SimpleDateFormat("MM-dd HH:mm:ss.SSS", java.util.Locale.US).format(java.util.Date(startMs))
        val p = ProcessBuilder("logcat", "-b", "crash", "-d", "-T", since).redirectErrorStream(true).start()
        val text = p.inputStream.bufferedReader().use { r -> r.readText().take(256 * 1024) }
        p.waitFor(5, TimeUnit.SECONDS)
        val lines = text.lines().filter { it.contains("Fatal signal") || it.contains("cmpbe") || it.contains("Abort message") }
        if (lines.isEmpty()) null else File.createTempFile("crash", ".log", app?.cacheDir).apply { writeText(lines.joinToString("\n")) }
    } catch (_: Exception) {
        null
    }

    private fun finish(s: PlayState, endMs: Long, end: End) {
        val key = s.appid ?: RUN_KEY
        open.remove(key)
        counting.remove(key)
        val ctx = app
        val scanner = LogSliceScanner()
        s.settings = settings(ctx)
        windows.remove(key)?.let { w ->
            s.display = LinkedHashMap(s.display).apply { put("window", if (w.size > 1) "mixed" else w.firstOrNull() ?: get("window")) }
        }
        val r = PlayRecord.base(s, endMs)
        val f = scanner.scan(s.sources, s.appid)
        if (ctx != null) {
            val extra = gameSources(ctx, s, f, scanner)
            if (extra.isNotEmpty()) scanner.scanInto(f, extra, s.appid)
        }
        if (s.vortekExit != null || f.has("crash") || f.has("device_lost")) {
            crashBuffer(s.startMs)?.let { tmp ->
                scanner.scanInto(f, listOf(LogSliceScanner.Source("crash buffer", tmp)), s.appid)
                tmp.delete()
            }
        }
        // The session stopping kills the server too (exit 137); only a death the component saw on its own counts.
        if (s.vortekExit == null) f.errors.remove("vortek")
        val used = f.gpuUsed ?: PlayRecord.gpuFromEnv(s.env)
        val evidence = PlayRecord.evidenceFrom(
            PlayRecord.evidence(s, endMs), f, usedVortek = used == "vortek",
            sessionStopped = end.stopped, sessionStatus = end.sessionStatus ?: end.runStatus, failureCode = end.failureCode,
        )
        val exit = ExitClassifier.classify(evidence)
        PlayRecord.finish(r, f, exit)
        journal?.append(r)
        val kept = keep(s.logs, exit.flags, endMs)
        lastEnded = Triple(s.appid ?: RUN_KEY, s.id, endMs)
        Log.i(TAG, "play ${s.id}: ${s.appid ?: ""} ${s.game} ${(endMs - s.startMs) / 60_000} min, ${exit.reason}" +
            (if (exit.flags.isNotEmpty()) " ${exit.flags}" else "") + (kept?.let { ", logs kept $it d" } ?: ""))
        checkpoint(endMs)
    }

    private fun keep(logs: String, flags: List<String>, now: Long): Long? {
        val (ms, why) = ExitClassifier.keepFor(flags) ?: return null
        val ctx = app ?: return null
        val internal = File(LinuxRuntime.logDir(ctx), logs)
        KeepMarker.extend(internal, now + ms, why)
        ctx.getExternalFilesDir(null)?.let { File(it, "logs/$logs") }?.takeIf { it.isDirectory }?.let { KeepMarker.extend(it, now + ms, why) }
        return ms / (24L * 3600 * 1000)
    }

    private fun checkpoint(now: Long) {
        lastCheckpointAt = now
        val j = journal ?: return
        if (open.isEmpty()) { j.writeOpen(null); return }
        val ctx = app
        val plays = open.values.map { s ->
            s.settings = settings(ctx)
            linkedMapOf(
                "id" to s.id,
                "appid" to s.appid,
                "record" to PlayRecord.base(s, now),
                "evidence" to PlayRecord.evidence(s, now),
                "sources" to s.sources.map { linkedMapOf("label" to it.label, "path" to it.file.path, "from" to it.from) },
            )
        }
        runCatching {
            j.writeOpen(linkedMapOf("v" to PlayRecord.VERSION, "appPid" to Process.myPid(), "written" to PlayRecord.iso(now),
                "logs" to dir?.name, "plays" to plays))
        }.onFailure { Log.w(TAG, "open.json", it) }
    }

    /**
     * At a session's start, before abandoned session folders are finished: records the plays a dead app left
     * in open.json, with the reason Android gives for the death, and keeps their session folder.
     */
    fun recover(context: Context) {
        val ctx = context.applicationContext
        val j = journalFor(ctx) ?: return
        lock.lock()
        try {
            val state = j.readOpen() ?: return
            if (running) return // this process's own session is still being tracked
            val pid = state.long("appPid")?.toInt()
            val exit = if (pid != null && pid != Process.myPid()) appExit(ctx, pid) else null
            val sameProcess = pid == Process.myPid()
            app = ctx
            for (p in state.list("plays").orEmpty().filterIsInstance<Map<*, *>>()) {
                runCatching { recoverPlay(ctx, j, p, exit, sameProcess) }.onFailure { Log.w(TAG, "recovering a play", it) }
            }
            j.writeOpen(null)
        } finally {
            lock.unlock()
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun recoverPlay(ctx: Context, j: JournalFile, p: Map<*, *>, exit: ExitClassifier.AppExit?, sameProcess: Boolean) {
        val record = (p.obj("record") as? Map<String, Any?>)?.let { LinkedHashMap(it) } ?: return
        val evidence = p.obj("evidence") ?: return
        val sources = p.list("sources").orEmpty().filterIsInstance<Map<*, *>>().mapNotNull { s ->
            val path = s.str("path") ?: return@mapNotNull null
            LogSliceScanner.Source(s.str("label") ?: File(path).name, File(path), s.long("from") ?: 0L)
        }
        val appid = p.long("appid")
        val f = LogSliceScanner().scan(sources, appid)
        val used = f.gpuUsed ?: (record.obj("gpu")?.str("used"))
        val ev = PlayRecord.evidenceFrom(evidence, f, usedVortek = used == "vortek",
            appKilled = exit, appGone = exit == null && !sameProcess, sessionStopped = sameProcess,
            failureCode = if (sameProcess) "not finished in time" else null)
        val e = ExitClassifier.classify(ev)
        PlayRecord.finish(record, f, e)
        record["recovered"] = true
        j.append(record)
        val logs = record.str("logs") ?: return
        val now = System.currentTimeMillis()
        val kept = keep(logs, e.flags, now)
        Log.i(TAG, "play ${record["id"]} recorded late: ${e.reason}" + (kept?.let { ", logs kept $it d" } ?: ""))
    }

    private fun appExit(ctx: Context, pid: Int): ExitClassifier.AppExit? {
        if (Build.VERSION.SDK_INT < 30) return null
        return runCatching {
            ctx.getSystemService(ActivityManager::class.java)?.getHistoricalProcessExitReasons(ctx.packageName, pid, 1)
                ?.firstOrNull { it.pid == pid }
                ?.let { ExitClassifier.AppExit(DeviceReport.exitReasonName(it.reason), it.status, it.description, it.timestamp) }
        }.getOrNull()
    }
}
