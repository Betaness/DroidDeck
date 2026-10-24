package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.ClusterSteer.Action
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ClusterSteerTest {
    @Test fun `a cluster is slower than the little cores at their full clock`() {
        // The Xring O3: mid cores capacity 825 up to 3.69 GHz, prime 1024 up to 4.36 GHz, little 479.
        assertTrue(ClusterSteer.slower(825, 556_800, 3_686_400, 479))
        assertTrue(ClusterSteer.slower(825, 1_296_000, 3_686_400, 479))
        assertFalse(ClusterSteer.slower(825, 2_304_000, 3_686_400, 479))
        assertTrue(ClusterSteer.slower(1024, 1_113_600, 4_358_400, 479))
        assertFalse(ClusterSteer.slower(1024, 2_400_000, 4_358_400, 479))
        assertFalse(ClusterSteer.slower(825, 1_000_000, 0, 479)) // no maximum read
    }

    @Test fun `threads move after three held seconds in a row`() {
        val d = ClusterSteer.Decider()
        assertEquals(Action.NONE, d.tick(true))
        assertEquals(Action.NONE, d.tick(true))
        assertEquals(Action.NONE, d.tick(false)) // a raised clock starts the count again
        assertEquals(Action.NONE, d.tick(true))
        assertEquals(Action.NONE, d.tick(true))
        assertEquals(Action.STEER, d.tick(true))
        assertTrue(d.steered)
    }

    @Test fun `moved threads get every core back after a minute and move again if still held`() {
        val d = ClusterSteer.Decider()
        repeat(ClusterSteer.HELD_S - 1) { d.tick(true) }
        assertEquals(Action.STEER, d.tick(true))
        repeat(ClusterSteer.RETRY_S - 1) { assertEquals(Action.NONE, d.tick(false)) }
        assertEquals(Action.RELEASE, d.tick(false))
        assertFalse(d.steered)
        repeat(ClusterSteer.HELD_S - 1) { assertEquals(Action.NONE, d.tick(true)) }
        assertEquals(Action.STEER, d.tick(true))
    }

    @Test fun `a cluster held again right after a retry doubles the next wait`() {
        val d = ClusterSteer.Decider()
        fun steerNow() { repeat(ClusterSteer.HELD_S - 1) { d.tick(true) }; assertEquals(Action.STEER, d.tick(true)) }
        fun stay(seconds: Int) { repeat(seconds - 1) { assertEquals(Action.NONE, d.tick(false)) }; assertEquals(Action.RELEASE, d.tick(false)) }
        steerNow()
        assertEquals(ClusterSteer.RETRY_S, d.retryAfter)
        stay(ClusterSteer.RETRY_S)
        steerNow()
        assertEquals(ClusterSteer.RETRY_S * 2, d.retryAfter)
        stay(ClusterSteer.RETRY_S * 2)
        steerNow()
        assertEquals(ClusterSteer.RETRY_S * 4, d.retryAfter)
        repeat(4) { stay(d.retryAfter); steerNow() }
        assertEquals(ClusterSteer.RETRY_MAX_S, d.retryAfter)
        // Free for longer than a minute after a retry: back to the shortest wait.
        stay(d.retryAfter)
        repeat(ClusterSteer.RETRY_S + 1) { assertEquals(Action.NONE, d.tick(false)) }
        steerNow()
        assertEquals(ClusterSteer.RETRY_S, d.retryAfter)
    }
}
