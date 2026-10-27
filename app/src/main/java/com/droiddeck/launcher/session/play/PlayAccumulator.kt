package com.droiddeck.launcher.session.play

import kotlin.math.roundToInt

class Sample(
    val tMs: Long,
    val focused: Boolean,
    val visible: Boolean,
    val suspended: Boolean,
    /** WaylandCompositor.gameFrameCount(): game-layer frames since the app started. */
    val frames: Long,
    val modeHz: Float? = null,
    /** What the session asked for (the requested mode, after the peak-refresh and mirror caps). */
    val wantedHz: Float? = null,
    /** PowerKeeper's thermal_limit_refresh_rate, through the shell probe; null when unknown. */
    val limitHz: Int? = null,
    val thermalStatus: Int? = null,
    val headroom: Float? = null,
    val skinC: Float? = null,
    val socC: Float? = null,
    val batteryC: Float? = null,
    val cpuMHz: IntArray? = null,
    val capMHz: IntArray? = null,
    val mibridge: Map<String, Int>? = null,
    /** WaylandCompositor.screenFrameCount(): frames the compositor composited (copy path, Steam UI); null when unknown. */
    val screenFrames: Long? = null,
) {
    val counting get() = focused && visible && !suspended
}

/**
 * [mali-g2] A play's samples, folded as they come: per-minute aggregates (minutes from the play's start) and
 * running sums and extremes, never the samples themselves. A four-hour play holds 240 small minute entries.
 *
 * An interval between two samples counts for fps and freezes when the game had focus and the session was
 * shown and running at its start and still shown and running at its end. Focus changes cut intervals
 * (the journal samples at each), visibility and suspension are seen only at samples.
 *
 * Frames shown in an interval are the more of the game layer's and the compositor's: a zero-copy game
 * presents on its own layer and the compositor composites next to nothing, a game drawn through X11 (copy
 * path, or paced while its window is taken as covered) is only seen as composited frames.
 */
class PlayAccumulator(val startMs: Long) {
    private class Minute {
        var frames = 0L
        var countingMs = 0L
        var hzSum = 0.0
        var hzMs = 0L
        var maxSkin: Float? = null
        var maxBattery: Float? = null
        var maxStatus = -1
        var gameAnonKb = 0L
        var vortekAnonKb = 0L
    }

    private val minutes = ArrayList<Minute>()
    private var prev: Sample? = null

    var countingMs = 0L; private set
    var focusedMs = 0L; private set
    /** Frames shown in counted time (see the class comment). */
    var countedFrames = 0L; private set
    /** Of which the game layer's, and the compositor's, each on its own. */
    var countedLayerFrames = 0L; private set
    var countedScreenFrames = 0L; private set
    var screenCounterSeen = false; private set
    var framesAfterWarmup = 0L; private set
    var countingAfterWarmupMs = 0L; private set
    var firstFrameAt: Long? = null; private set
    var lastFrameAt: Long? = null; private set
    private var zeroRunMs = 0L
    var longestZeroRunMs = 0L; private set
    /** When the longest run without frames started, ms after the play's start. */
    var longestZeroRunAtMs: Long? = null; private set
    /** The longest run without frames that started after the loading stretch ([LOADING_MS] into the play). */
    var longestZeroRunAfterLoadMs = 0L; private set
    private var zeroRunFrom = 0L

    private var hzMs = 0L
    private var at60Ms = 0L
    private val at60Why = linkedMapOf("thermal" to 0L, "app" to 0L, "system" to 0L, "unknown" to 0L)
    var hzSwitches = 0; private set

    var statusMax: Int? = null; private set
    var headroomMax: Float? = null; private set
    var skinMin: Float? = null; private set
    var skinMax: Float? = null; private set
    var socMax: Float? = null; private set
    var batteryMin: Float? = null; private set
    var batteryMax: Float? = null; private set

    private var cpuSum: LongArray? = null
    private var cpuN: IntArray? = null
    private var capMin: IntArray? = null

    private val miGranted = LinkedHashMap<String, Int>()
    private val miSeen = LinkedHashMap<String, Int>()

    var gameHwmKb = 0L; private set
    var gameRssMaxKb = 0L; private set
    var gameAnonMaxKb = 0L; private set
    var vortekAnonMaxKb = 0L; private set
    var vortekHwmKb = 0L; private set
    var appNativeMaxBytes = 0L; private set
    var availMinBytes: Long? = null; private set
    var lowMemory = false; private set

    /** A gap longer than this between samples (the app frozen, the device asleep) is not counted. */
    private val maxIntervalMs = 30_000L

    fun add(s: Sample) {
        val p = prev
        prev = s
        val minute = minute(s.tMs)
        s.skinC?.let { minute.maxSkin = maxOf(minute.maxSkin ?: it, it) }
        s.batteryC?.let { minute.maxBattery = maxOf(minute.maxBattery ?: it, it) }
        s.thermalStatus?.let { minute.maxStatus = maxOf(minute.maxStatus, it) }
        if (s.screenFrames != null) screenCounterSeen = true
        thermal(s)
        cpu(s)
        s.mibridge?.forEach { (k, r) ->
            miSeen[k] = (miSeen[k] ?: 0) + 1
            if (r == 0) miGranted[k] = (miGranted[k] ?: 0) + 1
        }
        if (p == null) return
        val dt = s.tMs - p.tMs
        if (dt <= 0 || dt > maxIntervalMs) return
        val dLayer = (s.frames - p.frames).coerceAtLeast(0)
        val dScreen = if (s.screenFrames != null && p.screenFrames != null) (s.screenFrames - p.screenFrames).coerceAtLeast(0) else 0L
        val df = maxOf(dLayer, dScreen)
        if (p.focused) {
            focusedMs += dt
            val hz = s.modeHz
            if (hz != null) {
                hzMs += dt
                val m = minute(p.tMs)
                m.hzSum += hz.toDouble() * dt
                m.hzMs += dt
                if (hz <= 61f) {
                    at60Ms += dt
                    val why = why60(s)
                    at60Why[why] = (at60Why[why] ?: 0L) + dt
                }
            }
        }
        if (p.counting && s.visible && !s.suspended) {
            countingMs += dt
            countedFrames += df
            countedLayerFrames += dLayer
            countedScreenFrames += dScreen
            if (df > 0) {
                if (firstFrameAt == null) firstFrameAt = p.tMs
                lastFrameAt = s.tMs
            }
            val m = minute(p.tMs)
            m.countingMs += dt
            m.frames += df
            if (firstFrameAt != null && p.tMs - startMs >= WARMUP_MS) {
                countingAfterWarmupMs += dt
                framesAfterWarmup += df
            }
            // Stuck: no game frame and under one composited frame a second (a cursor blink is not a frame).
            if (firstFrameAt != null && dLayer == 0L && dScreen * 1000 < dt) {
                if (zeroRunMs == 0L) zeroRunFrom = p.tMs
                zeroRunMs += dt
                if (zeroRunMs > longestZeroRunMs) {
                    longestZeroRunMs = zeroRunMs
                    longestZeroRunAtMs = zeroRunFrom - startMs
                }
                if (zeroRunFrom - startMs >= LOADING_MS) longestZeroRunAfterLoadMs = maxOf(longestZeroRunAfterLoadMs, zeroRunMs)
            } else zeroRunMs = 0
        } else zeroRunMs = 0
    }

    /**
     * Why the display was at 60 Hz. PowerKeeper's cap first (the session's wanted rate is itself capped by
     * peak_refresh_rate, which the system lowers when hot), then the session asking for it, then signs of
     * heat; "system" only when the shell probe saw no cap and a cool skin, else "unknown".
     */
    private fun why60(s: Sample): String = when {
        s.limitHz != null && s.limitHz in 1..61 -> "thermal"
        s.wantedHz != null && s.wantedHz > 0f && s.wantedHz <= 61f -> "app"
        hot(s) -> "thermal"
        s.limitHz != null || s.skinC != null -> "system"
        else -> "unknown"
    }

    /** The board is hot: Android's thermal status, the skin, a warm battery, or MiBridge refusing the CPU. */
    private fun hot(s: Sample): Boolean =
        (s.thermalStatus ?: 0) >= 1 || (s.skinC ?: 0f) >= 40f || (s.batteryC ?: 0f) >= 39f ||
            (s.mibridge?.get("cpu")?.let { it != 0 } ?: false)

    /** A display mode change seen as it happened (SessionActivity), between samples. */
    fun hzSwitched() { hzSwitches++ }

    private fun thermal(s: Sample) {
        s.thermalStatus?.let { statusMax = maxOf(statusMax ?: it, it) }
        s.headroom?.takeIf { !it.isNaN() }?.let { headroomMax = maxOf(headroomMax ?: it, it) }
        s.skinC?.let { skinMin = minOf(skinMin ?: it, it); skinMax = maxOf(skinMax ?: it, it) }
        s.socC?.let { socMax = maxOf(socMax ?: it, it) }
        s.batteryC?.let { batteryMin = minOf(batteryMin ?: it, it); batteryMax = maxOf(batteryMax ?: it, it) }
    }

    /** cpufreq values of 0 are reads that failed: left out, not averaged in. */
    private fun cpu(s: Sample) {
        s.cpuMHz?.let { f ->
            val sum = cpuSum?.takeIf { it.size == f.size } ?: LongArray(f.size).also { cpuSum = it; cpuN = IntArray(f.size) }
            val n = cpuN!!
            for (i in f.indices) if (f[i] > 0) { sum[i] += f[i].toLong(); n[i]++ }
        }
        s.capMHz?.let { c ->
            val min = capMin?.takeIf { it.size == c.size } ?: IntArray(c.size).also { capMin = it }
            for (i in c.indices) if (c[i] > 0 && (min[i] == 0 || c[i] < min[i])) min[i] = c[i]
        }
    }

    fun memory(
        gameHwmKb: Long? = null, gameRssKb: Long? = null, gameAnonKb: Long? = null,
        vortekAnonKb: Long? = null, vortekHwmKb: Long? = null,
        appNativeBytes: Long? = null, availBytes: Long? = null, lowMemory: Boolean? = null,
        atMs: Long? = null,
    ) {
        gameHwmKb?.let { this.gameHwmKb = maxOf(this.gameHwmKb, it) }
        gameRssKb?.let { gameRssMaxKb = maxOf(gameRssMaxKb, it) }
        gameAnonKb?.let { gameAnonMaxKb = maxOf(gameAnonMaxKb, it) }
        vortekAnonKb?.let { vortekAnonMaxKb = maxOf(vortekAnonMaxKb, it) }
        vortekHwmKb?.let { this.vortekHwmKb = maxOf(this.vortekHwmKb, it) }
        appNativeBytes?.let { appNativeMaxBytes = maxOf(appNativeMaxBytes, it) }
        availBytes?.let { availMinBytes = minOf(availMinBytes ?: it, it) }
        if (lowMemory == true) this.lowMemory = true
        if (atMs != null) {
            val m = minute(atMs)
            gameAnonKb?.let { m.gameAnonKb = maxOf(m.gameAnonKb, it) }
            vortekAnonKb?.let { m.vortekAnonKb = maxOf(m.vortekAnonKb, it) }
        }
    }

    private fun minute(tMs: Long): Minute {
        val i = ((tMs - startMs) / 60_000L).toInt().coerceIn(0, MAX_MINUTES - 1)
        while (minutes.size <= i) minutes.add(Minute())
        return minutes[i]
    }

    /** Where the frames shown came from: the game's own layer, or the compositor (X11 copy path, hidden window). */
    fun source(): String? = when {
        countedFrames == 0L -> null
        countedLayerFrames >= countedScreenFrames -> "game-layer"
        else -> "screen"
    }

    /** Mean fps over the counted time; null with under 10 s of it, or when nothing was shown. */
    fun meanFps(): Double? = countingMs.takeIf { it >= 10_000 && firstFrameAt != null }?.let { r1(countedFrames * 1000.0 / it) }

    fun meanFpsAfterWarmup(): Double? =
        countingAfterWarmupMs.takeIf { it >= 30_000 && firstFrameAt != null }?.let { r1(framesAfterWarmup * 1000.0 / it) }

    /** The compositor's own rate over the counted time (next to nothing for a zero-copy game). */
    fun composedFps(): Double? =
        countingMs.takeIf { it >= 10_000 && screenCounterSeen }?.let { r1(countedScreenFrames * 1000.0 / it) }

    fun perMinuteFps(): List<Double?> = minutes.map { m -> m.countingMs.takeIf { it >= 5_000 }?.let { r1(m.frames * 1000.0 / it) } }
    /** The display's time-weighted mean refresh rate per minute (60 and 120 Hz alternating reads ~90). */
    fun perMinuteHz(): List<Int?> = minutes.map { m -> m.hzMs.takeIf { it > 0 }?.let { (m.hzSum / it).roundToInt() } }
    fun perMinuteSkin(): List<Double?> = minutes.map { m -> m.maxSkin?.let { r1(it.toDouble()) } }
    fun perMinuteBattery(): List<Double?> = minutes.map { m -> m.maxBattery?.let { r1(it.toDouble()) } }
    fun perMinuteStatus(): List<Int?> = minutes.map { m -> m.maxStatus.takeIf { it >= 0 } }
    fun perMinuteGameAnonMB(): List<Long?> = minutes.map { m -> m.gameAnonKb.takeIf { it > 0 }?.let { (it + 512) / 1024 } }
    fun perMinuteVortekAnonMB(): List<Long?> = minutes.map { m -> m.vortekAnonKb.takeIf { it > 0 }?.let { (it + 512) / 1024 } }

    fun at60Share(): Double? = hzMs.takeIf { it > 0 }?.let { r2(at60Ms.toDouble() / it) }
    fun at60Why(): Map<String, Double> = at60Why.mapValues { (_, ms) -> if (hzMs > 0) r2(ms.toDouble() / hzMs) else 0.0 }

    fun cpuMeanMHz(): List<Int?>? {
        val sum = cpuSum ?: return null
        val n = cpuN ?: return null
        if (n.all { it == 0 }) return null
        return sum.indices.map { i -> if (n[i] > 0) (sum[i] / n[i]).toInt() else null }
    }
    fun cpuCapMinMHz(): List<Int?>? = capMin?.takeIf { c -> c.any { it > 0 } }?.map { it.takeIf { v -> v > 0 } }
    fun mibridgeShares(): Map<String, Double> = miSeen.mapValues { (k, n) -> r2((miGranted[k] ?: 0).toDouble() / n) }

    companion object {
        const val WARMUP_MS = 60_000L
        /** A game's first two minutes: splash screens and loads that show nothing for half a minute are common. */
        const val LOADING_MS = 120_000L
        const val MAX_MINUTES = 24 * 60
        fun r1(v: Double) = (v * 10).roundToInt() / 10.0
        fun r2(v: Double) = (v * 100).roundToInt() / 100.0
    }
}
