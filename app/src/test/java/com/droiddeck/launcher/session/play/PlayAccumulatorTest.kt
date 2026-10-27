package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PlayAccumulatorTest {
    private val t0 = 1_800_000_000_000L

    /** Samples every 5 s from [fromS] to [toS] at [fps], frames continuing from [frames0]. */
    private fun run(
        acc: PlayAccumulator, fromS: Int, toS: Int, fps: Int, frames0: Long, focused: Boolean = true,
        visible: Boolean = true, suspended: Boolean = false, modeHz: Float = 120f, wantedHz: Float = 120f,
        status: Int = 0, skin: Float? = null, cpuRefused: Boolean = false,
    ): Long {
        var frames = frames0
        var s = fromS
        while (s <= toS) {
            acc.add(Sample(t0 + s * 1000L, focused, visible, suspended, frames, modeHz, wantedHz, null, status, null, skin, null, 30f,
                intArrayOf(2000, 2800, 3100), intArrayOf(3600, 2400, 4200), mapOf("cpu" to if (cpuRefused) -1 else 0, "gpu" to 0)))
            frames += fps * 5L
            s += 5
        }
        return frames - fps * 5L
    }

    @Test fun perMinuteFpsAndMeanCountOnlyFocusedTime() {
        val acc = PlayAccumulator(t0)
        var f = run(acc, 0, 55, 60, 0)
        // Steam's menu over the game for a minute: 30 fps of Steam UI frames, not the game's.
        f = run(acc, 60, 115, 30, f + 300, focused = false)
        f = run(acc, 120, 180, 60, f + 150)
        val perMin = acc.perMinuteFps()
        assertEquals(60.0, perMin[0]!!, 0.01)
        assertNull("the minute spent in Steam's menu has no counted time", perMin[1])
        assertEquals(60.0, perMin[2]!!, 0.01)
        assertEquals(60.0, acc.meanFps()!!, 0.6)
        assertEquals(120L, acc.focusedMs / 1000)
    }

    @Test fun suspendedOrHiddenStretchesAreNotCounted() {
        val acc = PlayAccumulator(t0)
        var f = run(acc, 0, 30, 60, 0)
        f = run(acc, 35, 90, 0, f + 300, visible = false)
        run(acc, 95, 120, 60, f)
        assertEquals(0L, acc.longestZeroRunMs)
        assertEquals(60.0, acc.meanFps()!!, 0.01)
    }

    @Test fun thirtySecondsWithoutFramesAfterTheFirstIsAFreeze() {
        val acc = PlayAccumulator(t0)
        // Loading without frames first: not a freeze.
        var f = run(acc, 0, 60, 0, 0)
        f = run(acc, 65, 120, 60, f)
        f = run(acc, 125, 165, 0, f + 300)
        run(acc, 170, 200, 60, f + 300)
        assertEquals(40L, acc.longestZeroRunMs / 1000)
    }

    @Test fun sixtyHertzIsAttributedToTheAppTheHeatOrTheSystem() {
        val acc = PlayAccumulator(t0)
        var f = run(acc, 0, 60, 60, 0)                                   // 120 Hz
        f = run(acc, 65, 120, 60, f + 300, modeHz = 60f, wantedHz = 60f) // asked for 60 (fps cap)
        f = run(acc, 125, 180, 60, f + 300, modeHz = 60f, skin = 42f)    // hot
        f = run(acc, 185, 240, 60, f + 300, modeHz = 60f, cpuRefused = true) // MiBridge refusing the CPU: hot board
        run(acc, 245, 300, 60, f + 300, modeHz = 60f)                     // no reason seen
        val share = acc.at60Share()!!
        assertEquals(0.8, share, 0.03)
        val why = acc.at60Why()
        assertEquals(0.2, why.getValue("app"), 0.03)
        assertEquals(0.4, why.getValue("thermal"), 0.03)
        assertEquals(0.2, why.getValue("system"), 0.03)
        assertEquals(listOf(120, 60, 60, 60, 60, 60), acc.perMinuteHz())
        assertEquals(0.8, acc.mibridgeShares().getValue("cpu"), 0.02)
        assertEquals(1.0, acc.mibridgeShares().getValue("gpu"), 0.0)
        assertEquals(listOf(2000, 2800, 3100), acc.cpuMeanMHz())
        assertEquals(listOf(3600, 2400, 4200), acc.cpuCapMinMHz())
    }

    @Test fun lowFpsAfterTheFirstMinute() {
        val acc = PlayAccumulator(t0)
        var f = run(acc, 0, 60, 60, 0)
        run(acc, 65, 300, 12, f)
        assertEquals(12.0, acc.meanFpsAfterWarmup()!!, 0.5)
    }

    @Test fun gapsLongerThanThirtySecondsAreDropped() {
        val acc = PlayAccumulator(t0)
        acc.add(Sample(t0, true, true, false, 0))
        acc.add(Sample(t0 + 120_000, true, true, false, 0)) // the app was frozen for two minutes
        acc.add(Sample(t0 + 125_000, true, true, false, 300))
        assertEquals(5_000L, acc.countingMs)
        assertEquals(300L, acc.countedFrames)
    }

    @Test fun memoryKeepsPeaksAndLows() {
        val acc = PlayAccumulator(t0)
        acc.memory(gameHwmKb = 1000, gameRssKb = 900, availBytes = 5L shl 30)
        acc.memory(gameHwmKb = 2000, gameRssKb = 800, availBytes = 3L shl 30, lowMemory = false)
        assertEquals(2000L, acc.gameHwmKb)
        assertEquals(900L, acc.gameRssMaxKb)
        assertEquals(3L shl 30, acc.availMinBytes)
    }
}
