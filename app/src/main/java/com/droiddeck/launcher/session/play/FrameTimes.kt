package com.droiddeck.launcher.session.play

import kotlin.math.ceil
import kotlin.math.roundToInt

/**
 * [mali-g2] Frame-time histogram of the game layer, for the play journal.
 *
 * [record] is called by the compositor thread for every game frame (WaylandCompositor.onGameFrame) with the
 * gap since the previous one: one array increment, no allocation, no lock. The journal copies the array
 * ([snapshot]) when the game gains or loses focus and every few seconds, and adds up the differences of the
 * stretches it counts ([FrameStats.of] then reads percentiles from the sum). Reads race the writer by design:
 * a frame landing in the copy or the next one makes no difference to a percentile.
 *
 * Bins: 0.25 ms to 50 ms, 1 ms to 200 ms, 10 ms to 1 s, then gaps of a second or more ("stalls"):
 * 1 s bins to 10 s, 10 s bins to 60 s, and one for a minute and longer.
 */
object FrameTimes {
    private const val FINE = 200 // [0, 50 ms) by 250 us
    private const val MID = 150 // [50, 200 ms) by 1 ms
    private const val COARSE = 80 // [200 ms, 1 s) by 10 ms
    /** First stall bin: gaps of a second or more. */
    const val STALL = FINE + MID + COARSE
    const val BINS = STALL + 9 + 5 + 1

    private val counts = IntArray(BINS)

    @JvmStatic
    fun record(intervalNs: Long) {
        if (intervalNs <= 0) return
        counts[bin(intervalNs / 1000)]++
    }

    fun snapshot(): IntArray = counts.copyOf()

    fun bin(us: Long): Int = when {
        us < 50_000 -> (us / 250).toInt()
        us < 200_000 -> FINE + ((us - 50_000) / 1_000).toInt()
        us < 1_000_000 -> FINE + MID + ((us - 200_000) / 10_000).toInt()
        us < 10_000_000 -> STALL + ((us - 1_000_000) / 1_000_000).toInt()
        us < 60_000_000 -> STALL + 9 + ((us - 10_000_000) / 10_000_000).toInt()
        else -> BINS - 1
    }

    /** Lower edge of bin [i], in microseconds. */
    fun low(i: Int): Long = when {
        i < FINE -> i * 250L
        i < FINE + MID -> 50_000L + (i - FINE) * 1_000L
        i < STALL -> 200_000L + (i - FINE - MID) * 10_000L
        i < STALL + 9 -> 1_000_000L + (i - STALL) * 1_000_000L
        i < BINS - 1 -> 10_000_000L + (i - STALL - 9) * 10_000_000L
        else -> 60_000_000L
    }

    /** Upper edge of bin [i], in microseconds (the last bin is open; a minute is given). */
    fun high(i: Int): Long = if (i >= BINS - 1) 60_000_000L else low(i + 1)

    /** [now] - [before], element by element (clamped at 0: a wrapped counter is not a negative frame count). */
    fun delta(now: IntArray, before: IntArray): IntArray =
        IntArray(BINS) { i -> (now[i] - before[i]).coerceAtLeast(0) }

    fun addInto(sum: IntArray, delta: IntArray) {
        for (i in 0 until BINS) sum[i] += delta[i]
    }
}

/** Frame-time statistics read from a [FrameTimes] histogram (sums of deltas). */
class FrameStats(
    /** Frames below a second apart: what the percentiles describe. */
    val frames: Long,
    val ftP50: Double?,
    val ftP90: Double?,
    val ftP99: Double?,
    /** 1 % / 5 % low: 1000 / the mean of the slowest 1 % / 5 % of those frames. */
    val low1: Double?,
    val low5: Double?,
    val stalls: Int,
    /** Lower edge of the longest stall's bin, seconds. */
    val longestStallS: Double?,
) {
    companion object {
        /** Fewer frames than this and the percentiles say nothing. */
        const val MIN_FRAMES = 30

        fun of(hist: IntArray): FrameStats {
            var n = 0L
            for (i in 0 until FrameTimes.STALL) n += hist[i]
            var stalls = 0
            var longest: Double? = null
            for (i in FrameTimes.STALL until FrameTimes.BINS) {
                if (hist[i] > 0) {
                    stalls += hist[i]
                    longest = FrameTimes.low(i) / 1e6
                }
            }
            if (n < MIN_FRAMES) return FrameStats(n, null, null, null, null, null, stalls, longest)
            return FrameStats(
                n, percentile(hist, n, 0.50), percentile(hist, n, 0.90), percentile(hist, n, 0.99),
                low(hist, n, 0.01), low(hist, n, 0.05), stalls, longest,
            )
        }

        /** Frame time in ms at quantile [q], linear within the bin. */
        private fun percentile(hist: IntArray, n: Long, q: Double): Double {
            val target = q * n
            var seen = 0L
            for (i in 0 until FrameTimes.STALL) {
                val c = hist[i]
                if (c == 0) continue
                if (seen + c >= target) {
                    val within = ((target - seen) / c).coerceIn(0.0, 1.0)
                    val us = FrameTimes.low(i) + within * (FrameTimes.high(i) - FrameTimes.low(i))
                    return round2(us / 1000.0)
                }
                seen += c
            }
            return round2(FrameTimes.low(FrameTimes.STALL) / 1000.0)
        }

        /** fps from the mean frame time of the slowest [share] of frames (bin middles). */
        private fun low(hist: IntArray, n: Long, share: Double): Double {
            val want = ceil(n * share).toLong().coerceAtLeast(1)
            var taken = 0L
            var sumUs = 0.0
            for (i in FrameTimes.STALL - 1 downTo 0) {
                val c = hist[i]
                if (c == 0) continue
                val take = minOf(c.toLong(), want - taken)
                sumUs += take * (FrameTimes.low(i) + FrameTimes.high(i)) / 2.0
                taken += take
                if (taken >= want) break
            }
            return round1(1_000_000.0 / (sumUs / taken))
        }

        internal fun round1(v: Double) = (v * 10).roundToInt() / 10.0
        internal fun round2(v: Double) = (v * 100).roundToInt() / 100.0
    }
}
