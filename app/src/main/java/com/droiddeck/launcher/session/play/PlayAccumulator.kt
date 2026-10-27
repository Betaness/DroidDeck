package com.droiddeck.launcher.session.play

import kotlin.math.roundToInt

/** One look at the device during a play, every few seconds (and at focus changes). */
class Sample(
    val tMs: Long,
    /** The game has input focus (not Steam's menu or overlay). */
    val focused: Boolean,
    /** The session is on screen (activity shown, screen on). */
    val visible: Boolean,
    val suspended: Boolean,
    /** WaylandCompositor.gameFrameCount(): game-layer frames since the app started. */
    val frames: Long,
    /** The panel's physical mode. */
    val modeHz: Float? = null,
    /** What the session asked for (the requested mode, after the peak-refresh and mirror caps). */
    val wantedHz: Float? = null,
    /** PowerKeeper's thermal_limit_refresh_rate, through the shell probe; null when unknown. */
    val limitHz: Int? = null,
    /** PowerManager thermal status (0 none .. 6 shutdown). */
    val thermalStatus: Int? = null,
    val headroom: Float? = null,
    val skinC: Float? = null,
    val socC: Float? = null,
    val batteryC: Float? = null,
    /** Mean current clock of each cpufreq policy, MHz. */
    val cpuMHz: IntArray? = null,
    /** scaling_max_freq of each policy, MHz. */
    val capMHz: IntArray? = null,
    /** MiBridge's last answer per kind (0 granted). */
    val mibridge: Map<String, Int>? = null,
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
 */
class PlayAccumulator(val startMs: Long) {
    private class Minute {
        var frames = 0L
        var countingMs = 0L
        var minModeHz: Float? = null
        var maxSkin: Float? = null
        var maxBattery: Float? = null
        var maxStatus = -1
    }

    private val minutes = ArrayList<Minute>()
    private var prev: Sample? = null

    var countingMs = 0L; private set
    var focusedMs = 0L; private set
    var countedFrames = 0L; private set
    var framesAfterWarmup = 0L; private set
    var countingAfterWarmupMs = 0L; private set
    var firstFrameAt: Long? = null; private set
    var lastFrameAt: Long? = null; private set
    private var zeroRunMs = 0L
    var longestZeroRunMs = 0L; private set

    // Refresh rate, over focused time.
    private var hzMs = 0L
    private var at60Ms = 0L
    private val at60Why = linkedMapOf("thermal" to 0L, "app" to 0L, "system" to 0L)

    // Thermal.
    var statusMax: Int? = null; private set
    var headroomMax: Float? = null; private set
    var skinMin: Float? = null; private set
    var skinMax: Float? = null; private set
    var socMax: Float? = null; private set
    var batteryMin: Float? = null; private set
    var batteryMax: Float? = null; private set

    // CPU.
    private var cpuSum: LongArray? = null
    private var cpuN = 0
    private var capMin: IntArray? = null

    // MiBridge: samples granted / samples with an answer, per kind.
    private val miGranted = LinkedHashMap<String, Int>()
    private val miSeen = LinkedHashMap<String, Int>()

    // Memory (kB), from [memory].
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
        s.modeHz?.let { hz -> if (s.focused) minute.minModeHz = minute.minModeHz?.let { minOf(it, hz) } ?: hz }
        s.skinC?.let { minute.maxSkin = maxOf(minute.maxSkin ?: it, it) }
        s.batteryC?.let { minute.maxBattery = maxOf(minute.maxBattery ?: it, it) }
        s.thermalStatus?.let { minute.maxStatus = maxOf(minute.maxStatus, it) }
        thermal(s)
        cpu(s)
        s.mibridge?.forEach { (k, r) ->
            miSeen[k] = (miSeen[k] ?: 0) + 1
            if (r == 0) miGranted[k] = (miGranted[k] ?: 0) + 1
        }
        if (p == null) return
        val dt = s.tMs - p.tMs
        if (dt <= 0 || dt > maxIntervalMs) return
        val df = (s.frames - p.frames).coerceAtLeast(0)
        if (p.focused) {
            focusedMs += dt
            s.modeHz?.let { hz ->
                hzMs += dt
                if (hz <= 61f) {
                    at60Ms += dt
                    val why = when {
                        s.wantedHz != null && s.wantedHz > 0f && s.wantedHz <= 61f -> "app"
                        hot(s) -> "thermal"
                        else -> "system"
                    }
                    at60Why[why] = (at60Why[why] ?: 0L) + dt
                }
            }
        }
        if (df > 0) {
            if (firstFrameAt == null) firstFrameAt = p.tMs
            lastFrameAt = s.tMs
        }
        if (p.counting && s.visible && !s.suspended) {
            countingMs += dt
            countedFrames += df
            val m = minute(p.tMs)
            m.countingMs += dt
            m.frames += df
            if (firstFrameAt != null && p.tMs - startMs >= WARMUP_MS) {
                countingAfterWarmupMs += dt
                framesAfterWarmup += df
            }
            if (firstFrameAt != null && df == 0L) {
                zeroRunMs += dt
                longestZeroRunMs = maxOf(longestZeroRunMs, zeroRunMs)
            } else zeroRunMs = 0
        } else zeroRunMs = 0
    }

    /** The board is hot: Android's thermal status, PowerKeeper's cap, the skin, or MiBridge refusing the CPU. */
    private fun hot(s: Sample): Boolean =
        (s.limitHz != null && s.limitHz in 1..61) || (s.thermalStatus ?: 0) >= 1 || (s.skinC ?: 0f) >= 40f ||
            (s.mibridge?.get("cpu")?.let { it != 0 } ?: false)

    private fun thermal(s: Sample) {
        s.thermalStatus?.let { statusMax = maxOf(statusMax ?: it, it) }
        s.headroom?.takeIf { !it.isNaN() }?.let { headroomMax = maxOf(headroomMax ?: it, it) }
        s.skinC?.let { skinMin = minOf(skinMin ?: it, it); skinMax = maxOf(skinMax ?: it, it) }
        s.socC?.let { socMax = maxOf(socMax ?: it, it) }
        s.batteryC?.let { batteryMin = minOf(batteryMin ?: it, it); batteryMax = maxOf(batteryMax ?: it, it) }
    }

    private fun cpu(s: Sample) {
        s.cpuMHz?.let { f ->
            val sum = cpuSum?.takeIf { it.size == f.size } ?: LongArray(f.size).also { cpuSum = it; cpuN = 0 }
            for (i in f.indices) sum[i] += f[i].toLong()
            cpuN++
        }
        s.capMHz?.let { c ->
            val min = capMin?.takeIf { it.size == c.size } ?: c.copyOf().also { capMin = it }
            for (i in c.indices) if (c[i] in 1 until min[i]) min[i] = c[i]
        }
    }

    fun memory(
        gameHwmKb: Long? = null, gameRssKb: Long? = null, gameAnonKb: Long? = null,
        vortekAnonKb: Long? = null, vortekHwmKb: Long? = null,
        appNativeBytes: Long? = null, availBytes: Long? = null, lowMemory: Boolean? = null,
    ) {
        gameHwmKb?.let { this.gameHwmKb = maxOf(this.gameHwmKb, it) }
        gameRssKb?.let { gameRssMaxKb = maxOf(gameRssMaxKb, it) }
        gameAnonKb?.let { gameAnonMaxKb = maxOf(gameAnonMaxKb, it) }
        vortekAnonKb?.let { vortekAnonMaxKb = maxOf(vortekAnonMaxKb, it) }
        vortekHwmKb?.let { this.vortekHwmKb = maxOf(this.vortekHwmKb, it) }
        appNativeBytes?.let { appNativeMaxBytes = maxOf(appNativeMaxBytes, it) }
        availBytes?.let { availMinBytes = minOf(availMinBytes ?: it, it) }
        if (lowMemory == true) this.lowMemory = true
    }

    private fun minute(tMs: Long): Minute {
        val i = ((tMs - startMs) / 60_000L).toInt().coerceIn(0, MAX_MINUTES - 1)
        while (minutes.size <= i) minutes.add(Minute())
        return minutes[i]
    }

    /** Mean fps over the counted time; null with under 10 s of it. */
    fun meanFps(): Double? = countingMs.takeIf { it >= 10_000 }?.let { r1(countedFrames * 1000.0 / it) }

    /** Mean fps after the first minute (loading and the title screen left out). */
    fun meanFpsAfterWarmup(): Double? = countingAfterWarmupMs.takeIf { it >= 30_000 }?.let { r1(framesAfterWarmup * 1000.0 / it) }

    /** Per minute of the play: mean fps over its counted time (null with under 5 s counted). */
    fun perMinuteFps(): List<Double?> = minutes.map { m -> m.countingMs.takeIf { it >= 5_000 }?.let { r1(m.frames * 1000.0 / it) } }
    fun perMinuteHz(): List<Int?> = minutes.map { m -> m.minModeHz?.roundToInt() }
    fun perMinuteSkin(): List<Double?> = minutes.map { m -> m.maxSkin?.let { r1(it.toDouble()) } }
    fun perMinuteBattery(): List<Double?> = minutes.map { m -> m.maxBattery?.let { r1(it.toDouble()) } }
    fun perMinuteStatus(): List<Int?> = minutes.map { m -> m.maxStatus.takeIf { it >= 0 } }

    /** Share of focused time the panel ran at 60 Hz or less; null when the mode was never read. */
    fun at60Share(): Double? = hzMs.takeIf { it > 0 }?.let { r2(at60Ms.toDouble() / it) }
    fun at60Why(): Map<String, Double> = at60Why.mapValues { (_, ms) -> if (hzMs > 0) r2(ms.toDouble() / hzMs) else 0.0 }

    fun cpuMeanMHz(): List<Int>? = cpuSum?.takeIf { cpuN > 0 }?.map { (it / cpuN).toInt() }
    fun cpuCapMinMHz(): List<Int>? = capMin?.toList()
    fun mibridgeShares(): Map<String, Double> = miSeen.mapValues { (k, n) -> r2((miGranted[k] ?: 0).toDouble() / n) }

    companion object {
        const val WARMUP_MS = 60_000L
        const val MAX_MINUTES = 24 * 60
        fun r1(v: Double) = (v * 10).roundToInt() / 10.0
        fun r2(v: Double) = (v * 100).roundToInt() / 100.0
    }
}
