package com.droiddeck.launcher.input

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GuestPointerTrackTest {
    /** The compositor's side: deltas wait in its pipe until it reads them, then move and clamp its pointer. */
    private class Compositor(val w: Int, val h: Int, var x: Double, var y: Double) {
        var seq = 0
        /** A confined pointer's box (x0, y0, x1, y1), or null. */
        var box: DoubleArray? = null
        private val pipe = ArrayDeque<IntArray>()

        fun send(track: GuestPointerTrack, dx: Int, dy: Int, nowMs: Long) {
            val dx256 = dx * 256
            val dy256 = dy * 256
            pipe.addLast(intArrayOf(dx256, dy256, track.add(dx256, dy256, nowMs)))
        }

        fun read(n: Int = Int.MAX_VALUE) {
            var left = n
            while (left-- > 0) {
                val m = pipe.removeFirstOrNull() ?: return
                x = (x + m[0] / 256.0).coerceIn(0.0, w - 1.0)
                y = (y + m[1] / 256.0).coerceIn(0.0, h - 1.0)
                box?.let {
                    x = x.coerceIn(it[0], it[2])
                    y = y.coerceIn(it[1], it[3])
                }
                seq = m[2]
            }
        }

        /** A scene stretched over the whole output, as the Xiaomi Pad's half-resolution session. */
        fun state(map: DoubleArray = doubleArrayOf(1.0 / w, 0.0, 1.0 / h, 0.0)): DoubleArray =
            doubleArrayOf(x, y, w.toDouble(), h.toDouble(), map[0], map[1], map[2], map[3],
                (seq.toLong() and 0xffffffffL).toDouble()) +
                (box ?: doubleArrayOf(0.0, 0.0, w - 1.0, h - 1.0))
    }

    private val out = FloatArray(2)

    @Test fun theDrawnCursorMovesTwoViewPixelsPerCountInAHalfResolutionSession() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 782.0, 196.0)
        var now = 1000L
        var lastY = Float.NaN
        for (i in 0 until 700) {
            compositor.send(track, 0, 1, now)
            if (i % 4 == 3) compositor.read()
            assertTrue(track.place(compositor.state(), now, 3408, 2272, out))
            if (!lastY.isNaN()) assertEquals("count $i", 2f, out[1] - lastY, 1e-3f)
            assertEquals(1564f, out[0], 1e-3f)
            lastY = out[1]
            now += 2
        }
        assertEquals((196 + 700) * 2f, lastY, 1e-3f)
        compositor.read()
        assertTrue(track.place(compositor.state(), now + 60, 3408, 2272, out))
        assertEquals("no catch-up step once the compositor has read everything", lastY, out[1], 1e-3f)
        assertEquals(0, track.pending())
    }

    @Test fun motionTheCompositorHasNotReadStopsAtTheSceneEdge() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 400.0, 1130.0)
        repeat(10) { compositor.send(track, 0, 1, 0) }
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(1135 * 2f, out[1], 1e-3f)
        repeat(10) { compositor.send(track, 0, -1, 0) }
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals("back up from the edge, not from beyond it", 1125 * 2f, out[1], 1e-3f)
        compositor.read()
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(1125 * 2f, out[1], 1e-3f)
    }

    @Test fun motionTheCompositorHasNotReadStopsAtTheConfinedBox() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 595.0, 300.0)
        compositor.box = doubleArrayOf(300.0, 150.0, 599.0, 349.0)
        repeat(10) { compositor.send(track, 1, 0, 0) }
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(599 * 2f, out[0], 1e-3f)
        repeat(4) { compositor.send(track, -1, 0, 0) }
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals("back from the box edge", 595 * 2f, out[0], 1e-3f)
        compositor.read()
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals("the same place once the compositor has read it", 595 * 2f, out[0], 1e-3f)
    }

    @Test fun aWarpTheCompositorPublishedMovesTheCursorWithoutMotion() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        compositor.x = 852.0
        compositor.y = 568.0
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(1704f, out[0], 1e-3f)
        assertEquals(1136f, out[1], 1e-3f)
    }

    @Test fun deltasTheCompositorHasReadAreNotAddedAgain() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        repeat(5) { compositor.send(track, 3, 0, 0) }
        compositor.read(5)
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(115 * 2f, out[0], 1e-3f)
        assertEquals(0, track.pending())
    }

    @Test fun beforeTheFirstReadDeltaEverythingSentCounts() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        repeat(3) { compositor.send(track, 1, 0, 0) }
        assertEquals(0, compositor.seq)
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(103 * 2f, out[0], 1e-3f)
    }

    @Test fun aDeltaNotReadWithinMaxAgeIsForgotten() {
        val track = GuestPointerTrack(maxAgeMs = 500)
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        compositor.send(track, 10, 0, 1000)
        assertTrue(track.place(compositor.state(), 1500, 3408, 2272, out))
        assertEquals(110 * 2f, out[0], 1e-3f)
        assertTrue(track.place(compositor.state(), 1501, 3408, 2272, out))
        assertEquals(100 * 2f, out[0], 1e-3f)
        assertEquals(0, track.pending())
    }

    @Test fun theOldestDeltasGoWhenTheTrackIsFull() {
        val track = GuestPointerTrack(capacity = 4)
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        repeat(6) { compositor.send(track, 1, 0, 0) }
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(104 * 2f, out[0], 1e-3f)
        compositor.read(2)
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(106 * 2f, out[0], 1e-3f)
    }

    @Test fun numbersWrapAndSkipZero() {
        val track = GuestPointerTrack()
        track.lastSeq = -2
        val compositor = Compositor(1704, 1136, 100.0, 100.0)
        compositor.send(track, 1, 0, 0)
        assertEquals(-1, track.lastSeq)
        compositor.send(track, 1, 0, 0)
        assertEquals(1, track.lastSeq)
        compositor.read(1)
        assertEquals("published as an unsigned 32-bit number", 4294967295.0, compositor.state()[GuestPointerTrack.SEQ], 0.0)
        assertTrue(track.place(compositor.state(), 0, 3408, 2272, out))
        assertEquals(102 * 2f, out[0], 1e-3f)
        assertEquals(1, track.pending())
    }

    @Test fun aLetterboxedSceneUsesTheCompositorsMapping() {
        val track = GuestPointerTrack()
        val k = 3408.0 / 1280
        val compositor = Compositor(1280, 720, 640.0, 360.0)
        val map = doubleArrayOf(k / 3408, 0.0, k / 2272, 177.0 / 2272)
        assertTrue(track.place(compositor.state(map), 0, 3408, 2272, out))
        assertEquals(1704f, out[0], 1e-3f)
        assertEquals((177 + 360 * k).toFloat(), out[1], 1e-3f)
        compositor.send(track, 1, 0, 0)
        assertTrue(track.place(compositor.state(map), 0, 3408, 2272, out))
        assertEquals((1704 + k).toFloat(), out[0], 1e-3f)
    }

    @Test fun aSmallerViewScalesTheSamePlace() {
        val track = GuestPointerTrack()
        val compositor = Compositor(1704, 1136, 852.0, 568.0)
        assertTrue(track.place(compositor.state(), 0, 1704, 1136, out))
        assertEquals(852f, out[0], 1e-3f)
        assertEquals(568f, out[1], 1e-3f)
    }

    @Test fun noSceneOrViewNoPlace() {
        val track = GuestPointerTrack()
        val state = Compositor(1704, 1136, 1.0, 1.0).state()
        assertFalse(track.place(state, 0, 0, 2272, out))
        assertFalse(track.place(state.copyOf().also { it[GuestPointerTrack.SCENE_W] = 0.0 }, 0, 3408, 2272, out))
        assertFalse("an older layout", track.place(state.copyOf(9), 0, 3408, 2272, out))
    }
}
