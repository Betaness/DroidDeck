package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FrameTimesTest {
    private fun hist(vararg msCounts: Pair<Double, Int>): IntArray {
        val h = IntArray(FrameTimes.BINS)
        for ((ms, n) in msCounts) h[FrameTimes.bin((ms * 1000).toLong())] += n
        return h
    }

    @Test fun binEdgesMeetAndCoverEveryRange() {
        assertEquals(0, FrameTimes.bin(0))
        assertEquals(199, FrameTimes.bin(49_999))
        assertEquals(200, FrameTimes.bin(50_000))
        assertEquals(349, FrameTimes.bin(199_999))
        assertEquals(350, FrameTimes.bin(200_000))
        assertEquals(FrameTimes.STALL - 1, FrameTimes.bin(999_999))
        assertEquals(FrameTimes.STALL, FrameTimes.bin(1_000_000))
        assertEquals(FrameTimes.BINS - 1, FrameTimes.bin(3_600_000_000))
        for (i in 0 until FrameTimes.BINS - 1) {
            assertEquals(i, FrameTimes.bin(FrameTimes.low(i)))
            assertEquals(i + 1, FrameTimes.bin(FrameTimes.high(i)))
        }
    }

    @Test fun steady60FpsReadsAs60() {
        val s = FrameStats.of(hist(16.6 to 1000))
        assertEquals(1000L, s.frames)
        assertEquals(16.6, s.ftP50!!, 0.25)
        assertEquals(16.6, s.ftP99!!, 0.25)
        assertEquals(60.0, s.low1!!, 1.0)
        assertEquals(0, s.stalls)
        assertNull(s.longestStallS)
    }

    @Test fun slowFramesShowInTheTailNotTheMedian() {
        // 980 frames at 12.5 ms, 15 at 25 ms, 5 at 100 ms.
        val s = FrameStats.of(hist(12.5 to 980, 25.0 to 15, 100.0 to 5))
        assertEquals(12.5, s.ftP50!!, 0.25)
        assertEquals(12.5, s.ftP90!!, 0.25)
        assertTrue("p99 ${s.ftP99}", s.ftP99!! in 25.0..26.0)
        // Slowest 1 %: 5 at 100 ms and 5 at 25 ms -> 62.5 ms -> 16 fps.
        assertEquals(16.0, s.low1!!, 0.3)
        // Slowest 5 %: 5 x 100 + 15 x 25 + 30 x 12.5 = 1250 ms / 50.
        assertEquals(40.0, s.low5!!, 0.5)
    }

    @Test fun stallsAreCountedApartFromThePercentiles() {
        val s = FrameStats.of(hist(16.6 to 600, 3400.0 to 1, 12_000.0 to 1))
        assertEquals(600L, s.frames)
        assertEquals(2, s.stalls)
        assertEquals(10.0, s.longestStallS!!, 0.0)
        assertEquals(60.0, s.low1!!, 1.0)
    }

    @Test fun tooFewFramesGiveNoPercentiles() {
        val s = FrameStats.of(hist(16.6 to 10))
        assertEquals(10L, s.frames)
        assertNull(s.ftP50)
        assertNull(s.low1)
    }

    @Test fun deltasAddUpOnlyTheCountedStretches() {
        val sum = IntArray(FrameTimes.BINS)
        val a = hist(16.6 to 100)
        val b = hist(16.6 to 160, 33.3 to 10) // 60 + 10 more while focused
        val c = hist(16.6 to 400, 33.3 to 10) // 240 more while the Steam menu was up: not added
        FrameTimes.addInto(sum, FrameTimes.delta(b, a))
        assertEquals(70, sum.sum())
        assertEquals(240, FrameTimes.delta(c, b).sum())
        // A counter that went back (not expected) never gives negative frames.
        assertEquals(0, FrameTimes.delta(a, c).sum())
    }

    @Test fun recordGoesToTheLiveHistogram() {
        val before = FrameTimes.snapshot()
        FrameTimes.record(16_600_000)
        FrameTimes.record(0) // the first frame has no gap: ignored
        val d = FrameTimes.delta(FrameTimes.snapshot(), before)
        assertEquals(1, d.sum())
        assertEquals(1, d[FrameTimes.bin(16_600)])
    }
}
