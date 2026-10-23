package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.ClusterSteer.Action
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ClusterSteerTest {
    @Test fun `a clock in the bottom quarter of its range is held`() {
        // The Xring O3's mid cores: 556.8 MHz to 3.69 GHz.
        assertTrue(ClusterSteer.held(556_800, 556_800, 3_686_400))
        assertTrue(ClusterSteer.held(1_142_400, 556_800, 3_686_400))
        assertFalse(ClusterSteer.held(1_804_800, 556_800, 3_686_400))
        assertFalse(ClusterSteer.held(3_686_400, 556_800, 3_686_400))
        assertFalse(ClusterSteer.held(1_000_000, 1_000_000, 1_000_000)) // a fixed clock is not held
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
}
