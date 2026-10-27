package com.droiddeck.launcher.session.play

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

class PlayState(
    val id: String,
    val logs: String,
    val appid: Long?,
    val game: String?,
    val exe: String?,
    val launch: String,
    val startMs: Long,
    val relaunchOf: String? = null,
) {
    val acc = PlayAccumulator(startMs)
    val hist = IntArray(FrameTimes.BINS)
    var pids: Set<Int> = emptySet()
    var gamePid = -1
    var gameProcess: String? = null
    /** [gamePid] is the busiest Windows process, not one named like the exe: looked at again now and then. */
    var gameGuessed = false
    var guessedAt = 0L
    var env: Map<String, String>? = null
    var app: Map<String, Any?> = emptyMap()
    var gpuAsked: String? = null
    var settings: Map<String, Any?> = emptyMap()
    var display: Map<String, Any?> = emptyMap()
    val marks = ArrayList<Pair<Long, Long>>() // (seconds into the play, wall time)
    var vortekExit: Int? = null
    var vortekExitAtMs: Long? = null
    var shellSeen = false
    var endMs: Long? = null
    var sources: List<LogSliceScanner.Source> = emptyList()
    /** What happened when, in seconds into the play: focus, refresh rate, heat, marks, the server's death. */
    val timeline = ArrayList<MutableList<Any?>>()
    var timelineDropped = 0
    /** The game's own logs found so far (label, file). */
    @Volatile var gameLogs: List<Pair<String, java.io.File>> = emptyList()
    var wasFocused = true
    var wasShown = true
    /** Wall time minus CLOCK_MONOTONIC when the play started, for the X11 layers' "(t=...)" stamps. */
    var uptimeToWallMs: Long? = null

    fun event(atMs: Long, what: String) {
        val atS = ((atMs - startMs) / 1000).coerceAtLeast(0)
        // Refresh-rate switches every few seconds (PowerKeeper hovering at its limit) fold into one entry a minute.
        val last = timeline.lastOrNull()
        if (what.startsWith("hz ") && last != null && (last[1] as String).startsWith("hz ") && atS - (last[0] as Long) < 60) {
            val n = (last.getOrNull(2) as? Int ?: 1) + 1
            last[1] = what
            if (last.size > 2) last[2] = n else last.add(n)
            return
        }
        if (timeline.size >= MAX_TIMELINE) { timelineDropped++; return }
        timeline.add(mutableListOf(atS, what))
    }

    companion object {
        const val MAX_TIMELINE = 48
    }
}

/**
 * [mali-g2] Builds the journal's records. Field names are fixed: tools/play-journal.py reads them, and they
 * stay clear of words LogRedactor treats as secrets (session, token, user, account, login, serial, mac...).
 * That is why the session folder is "logs" and a play's id is "<folder>#<n>".
 */
object PlayRecord {
    const val VERSION = 1

    fun iso(ms: Long, zone: TimeZone = TimeZone.getDefault()): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ssXXX", Locale.US).apply { timeZone = zone }.format(Date(ms))

    private fun mb(kb: Long): Long? = kb.takeIf { it > 0 }?.let { (it + 512) / 1024 }

    fun base(s: PlayState, endMs: Long, zone: TimeZone = TimeZone.getDefault()): LinkedHashMap<String, Any?> {
        val a = s.acc
        val stats = FrameStats.of(s.hist)
        val r = LinkedHashMap<String, Any?>()
        r["v"] = VERSION
        r["type"] = "play"
        r["id"] = s.id
        r["logs"] = s.logs
        r["app"] = s.app
        r["appid"] = s.appid
        r["game"] = s.game
        r["exe"] = s.exe
        r["launch"] = s.launch
        r["relaunchOf"] = s.relaunchOf
        r["start"] = iso(s.startMs, zone)
        r["end"] = iso(endMs, zone)
        r["durS"] = ((endMs - s.startMs) / 1000).coerceAtLeast(0)
        r["focusedS"] = a.focusedMs / 1000
        r["exit"] = null
        r["flags"] = emptyList<String>()
        r["gpu"] = linkedMapOf("asked" to s.gpuAsked, "used" to gpuFromEnv(s.env), "renderer" to null, "dxvk" to null)
        r["env"] = s.env
        r["settings"] = s.settings
        r["display"] = LinkedHashMap(s.display)
        val source = a.source()
        // Frame times are the game layer's: they say nothing about a game drawn through X11.
        val layer = source == "game-layer"
        r["fps"] = linkedMapOf(
            "source" to source,
            "mean" to a.meanFps(),
            "meanAfter1m" to a.meanFpsAfterWarmup(),
            "low1" to stats.low1.takeIf { layer },
            "low5" to stats.low5.takeIf { layer },
            "ftP50" to stats.ftP50.takeIf { layer },
            "ftP90" to stats.ftP90.takeIf { layer },
            "ftP99" to stats.ftP99.takeIf { layer },
            "frames" to a.countedFrames,
            "layerFrames" to a.countedLayerFrames,
            "composedFps" to a.composedFps(),
            "countedS" to a.countingMs / 1000,
            "stalls" to stats.stalls.takeIf { layer },
            "longestStallS" to stats.longestStallS.takeIf { layer },
            "freezeS" to a.longestZeroRunMs / 1000,
            "freezeAtS" to a.longestZeroRunAtMs?.takeIf { a.longestZeroRunMs >= 30_000 }?.let { it / 1000 },
            "perMin" to a.perMinuteFps(),
        )
        r["hz"] = linkedMapOf("at60" to a.at60Share(), "why" to a.at60Why(), "switches" to a.hzSwitches, "perMin" to a.perMinuteHz())
        r["thermal"] = linkedMapOf(
            "statusMax" to a.statusMax?.let(::thermalName),
            "headroomMax" to a.headroomMax?.let { PlayAccumulator.r2(it.toDouble()) },
            "skinC" to pair(a.skinMin, a.skinMax),
            "socCMax" to a.socMax?.let { PlayAccumulator.r1(it.toDouble()) },
            "batteryC" to pair(a.batteryMin, a.batteryMax),
            "skinPerMin" to a.perMinuteSkin().takeIf { l -> l.any { it != null } },
            "batteryPerMin" to a.perMinuteBattery(),
            "statusPerMin" to a.perMinuteStatus(),
            "source" to if (s.shellSeen) "shell" else "app",
        )
        r["cpu"] = linkedMapOf("meanMHz" to a.cpuMeanMHz(), "capMinMHz" to a.cpuCapMinMHz(), "steers" to null)
        r["mibridge"] = a.mibridgeShares()
        r["mem"] = linkedMapOf(
            "game" to s.gameProcess,
            "gameHwmMB" to mb(a.gameHwmKb),
            "gameRssMaxMB" to mb(a.gameRssMaxKb),
            "gameAnonMaxMB" to mb(a.gameAnonMaxKb),
            "vortekAnonMaxMB" to mb(a.vortekAnonMaxKb),
            "vortekHwmMB" to mb(a.vortekHwmKb),
            "appNativeMaxMB" to a.appNativeMaxBytes.takeIf { it > 0 }?.let { it shr 20 },
            "availMinMB" to a.availMinBytes?.let { it shr 20 },
            "lowMemory" to a.lowMemory,
            "anonPerMin" to a.perMinuteGameAnonMB().takeIf { l -> l.any { it != null } },
            "vortekAnonPerMin" to a.perMinuteVortekAnonMB().takeIf { l -> l.any { it != null } },
        )
        r["errors"] = emptyList<Any>()
        r["marks"] = s.marks.map { (atS, t) -> linkedMapOf("atS" to atS, "t" to iso(t, zone)) }
        r["timeline"] = s.timeline.map { ArrayList(it) }
        if (s.timelineDropped > 0) r["timelineDropped"] = s.timelineDropped
        return r
    }

    private fun pair(min: Float?, max: Float?): List<Double>? =
        if (min == null || max == null) null else listOf(PlayAccumulator.r1(min.toDouble()), PlayAccumulator.r1(max.toDouble()))

    fun thermalName(status: Int): String = when (status) {
        0 -> "NONE"; 1 -> "LIGHT"; 2 -> "MODERATE"; 3 -> "SEVERE"; 4 -> "CRITICAL"; 5 -> "EMERGENCY"; 6 -> "SHUTDOWN"
        else -> "STATUS_$status"
    }

    fun gpuFromEnv(env: Map<String, String>?): String? {
        val icd = env?.get("VK_ICD_FILENAMES") ?: env?.get("VK_DRIVER_FILES") ?: return null
        return when {
            icd.contains("hybris") -> "hybris"
            icd.contains("vortek") -> "vortek"
            else -> null
        }
    }

    /** What the classifier needs from the samples, kept in open.json for a play finished late. */
    fun evidence(s: PlayState, endMs: Long): LinkedHashMap<String, Any?> {
        val stats = FrameStats.of(s.hist)
        val a = s.acc
        val layer = a.source() == "game-layer"
        return linkedMapOf(
            "startMs" to s.startMs,
            "durS" to (endMs - s.startMs) / 1000,
            "gameFrames" to a.countedFrames,
            "zeroRunS" to a.longestZeroRunMs / 1000,
            "zeroRunAfterLoadS" to a.longestZeroRunAfterLoadMs / 1000,
            "screenCounted" to a.screenCounterSeen,
            "longestStallS" to stats.longestStallS.takeIf { layer },
            "meanAfter1m" to a.meanFpsAfterWarmup(),
            "low1" to stats.low1.takeIf { layer },
            "fpsLimit" to (s.display["fpsLimit"] as? Number)?.toInt(),
            "at60" to a.at60Share(),
            "at60Thermal" to a.at60Why()["thermal"],
            "marked" to s.marks.isNotEmpty(),
            "vortekExit" to s.vortekExit,
            "vortekToEndS" to s.vortekExitAtMs?.let { (endMs - it) / 1000 },
            "uptimeToWallMs" to s.uptimeToWallMs,
            "runMode" to (s.launch != "steam"),
        )
    }

    fun evidenceFrom(
        m: Map<*, *>, findings: LogSliceScanner.Findings?, usedVortek: Boolean,
        appKilled: ExitClassifier.AppExit? = null, appGone: Boolean = false,
        sessionStopped: Boolean = false, sessionStatus: Int? = null, failureCode: String? = null,
    ) = ExitClassifier.Evidence(
        durS = m.long("durS") ?: 0L,
        appKilled = appKilled,
        appGone = appGone,
        sessionStopped = sessionStopped,
        sessionStatus = sessionStatus,
        failureCode = failureCode,
        vortekExit = m.long("vortekExit")?.toInt(),
        vortekToEndS = m.long("vortekToEndS"),
        usedVortek = usedVortek,
        findings = findings,
        steamExitCode = findings?.steamExitCode,
        gameFrames = m.long("gameFrames"),
        // With the compositor's live count, gameFrames holds composited frames already; the [stats] lines are the whole session's.
        screenFramesSeen = m["screenCounted"] != true && (findings?.screenFrames?.any { it > 0 } ?: false),
        longestZeroRunS = m.long("zeroRunS") ?: 0L,
        zeroRunAfterLoadS = m.long("zeroRunAfterLoadS") ?: m.long("zeroRunS") ?: 0L,
        longestStallS = m.dbl("longestStallS"),
        meanFpsAfterWarmup = m.dbl("meanAfter1m"),
        low1 = m.dbl("low1"),
        at60 = m.dbl("at60"),
        at60Thermal = m.dbl("at60Thermal"),
        marked = m["marked"] == true,
        runMode = m["runMode"] == true,
        fpsLimit = m.long("fpsLimit")?.toInt(),
    )

    /** Kinds whose first time in the game's logs goes on the timeline. */
    private val TIMELINE_KINDS = listOf("hidden", "x11_copy", "unsupported", "device_lost", "oom", "crash", "vortek")

    @Suppress("UNCHECKED_CAST")
    fun finish(
        r: MutableMap<String, Any?>, f: LogSliceScanner.Findings?, exit: ExitClassifier.Exit,
        zone: TimeZone = TimeZone.getDefault(), startMs: Long? = null,
    ) {
        r["exit"] = linkedMapOf(
            "reason" to exit.reason,
            "steamCode" to exit.steamCode,
            "signal" to exit.signal,
            "vortek" to exit.vortek,
            "appExit" to exit.appExit?.let { a ->
                linkedMapOf("reason" to a.reason, "status" to a.status, "description" to a.description, "t" to iso(a.atMs, zone))
            },
            "detail" to exit.detail,
        )
        r["flags"] = exit.flags
        if (f == null) return
        val fpsSource = (r["fps"] as? Map<String, Any?>)?.get("source")
        (r["gpu"] as? Map<String, Any?>)?.let { g ->
            r["gpu"] = LinkedHashMap(g).apply {
                if (f.gpuUsed != null) put("used", f.gpuUsed)
                if (f.software) put("used", "cpu")
                put("renderer", f.renderer ?: f.device)
                put("dxvk", f.dxvk)
            }
        }
        (r["display"] as? Map<String, Any?>)?.let { d ->
            r["display"] = LinkedHashMap(d).apply {
                val chains = f.swapchains()
                put("swapchain", chains.firstOrNull())
                if (chains.size > 1) put("swapchains", chains.take(5))
                put("present", listOfNotNull(
                    "zero-copy".takeIf { fpsSource == "game-layer" },
                    "x11-copy".takeIf { fpsSource == "screen" || f.inGame("x11_copy") },
                    "hidden-paced".takeIf { f.inGame("hidden") },
                ).joinToString("+").ifEmpty { null })
            }
        }
        (r["fps"] as? Map<String, Any?>)?.let { fps ->
            r["fps"] = LinkedHashMap(fps).apply {
                // The [stats] lines' rate covers the whole slice (Steam's UI, after the game): only a stand-in.
                if (get("composedFps") == null && f.screenFrames.isNotEmpty()) put("statsFps", PlayAccumulator.r1(f.screenFrames.average() / 10.0))
                if (f.x11Fps.isNotEmpty()) put("x11Fps", PlayAccumulator.r1(f.x11Fps.average()))
                if (get("source") == null) put("source", when {
                    f.x11Fps.isNotEmpty() -> "x11-stats"
                    f.screenFrames.isNotEmpty() -> "stats"
                    else -> null
                })
            }
        }
        (r["cpu"] as? Map<String, Any?>)?.let { c -> r["cpu"] = LinkedHashMap(c).apply { put("steers", f.steers) } }
        fun atS(ms: Long?): Long? = if (ms == null || startMs == null) null else (ms - startMs) / 1000
        r["errors"] = f.errors.map { (k, v) ->
            linkedMapOf("k" to k, "n" to v.n, "atS" to atS(v.firstAtMs), "game" to f.inGame(k), "first" to v.first)
        }
        r["scanned"] = f.sources.distinct()
        if (startMs != null) {
            val events = (r["timeline"] as? List<*>)?.filterIsInstance<List<Any?>>()?.map { ArrayList(it) } ?: emptyList()
            val more = TIMELINE_KINDS.mapNotNull { k ->
                val at = atS(f.errors[k]?.firstAtMs)?.takeIf { it >= 0 && f.inGame(k) } ?: return@mapNotNull null
                arrayListOf<Any?>(at, k)
            }
            if (more.isNotEmpty()) r["timeline"] = (events + more).sortedBy { (it[0] as Number).toLong() }
        }
    }
}
