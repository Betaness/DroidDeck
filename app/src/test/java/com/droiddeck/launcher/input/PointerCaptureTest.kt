package com.droiddeck.launcher.input

import org.junit.Assert.*
import org.junit.Test

class PointerCaptureTest {
    private class Case(bits: Int) {
        val v = BooleanArray(13) { bits and (1 shl it) != 0 }
        val mouse get() = v[0]
        val locked get() = v[1]
        val multiWindow get() = v[2]
        val glasses get() = v[3]
        val resumed get() = v[4]
        val focused get() = v[5]
        val drawer get() = v[6]
        val pcKeyboard get() = v[7]
        val achievements get() = v[8]
        val inputSettings get() = v[9]
        val pip get() = v[10]
        val bottom get() = v[11]
        val touchEditor get() = v[12]

        fun blocker() = PointerCapturePolicy.blocker(
            mouseAttached = mouse, pointerLocked = locked, multiWindow = multiWindow, glassesOutput = glasses,
            resumed = resumed, windowFocused = focused, drawerOpen = drawer, pcKeyboardOpen = pcKeyboard,
            achievementsOpen = achievements, inputSettingsOpen = inputSettings, pipUi = pip,
            bottomOutput = bottom, touchEditorOpen = touchEditor,
        )

        /** The condition SessionActivity used before the policy was extracted. */
        fun wantedBefore(): Boolean {
            val relativeMouse = !multiWindow && !glasses && mouse
            return (locked || relativeMouse) && resumed && focused && !drawer && !pcKeyboard && !achievements &&
                !inputSettings && !pip && !bottom && !touchEditor && mouse
        }
    }

    private fun wanted(vararg set: Int): Case {
        var bits = (1 shl 0) or (1 shl 4) or (1 shl 5)
        for (b in set) bits = bits xor (1 shl b)
        return Case(bits)
    }

    @Test fun wantsCaptureExactlyWhenTheOldConditionDid() {
        for (bits in 0 until (1 shl 13)) {
            val case = Case(bits)
            assertEquals("case $bits", case.wantedBefore(), case.blocker() == null)
        }
    }

    @Test fun namesWhatStopsCapture() {
        assertNull(wanted().blocker())
        assertEquals("paused", wanted(4).blocker())
        assertEquals("no window focus", wanted(5).blocker())
        assertEquals("drawer open", wanted(6).blocker())
        assertEquals("PC keyboard open", wanted(7).blocker())
        assertEquals("achievements open", wanted(8).blocker())
        assertEquals("input settings open", wanted(9).blocker())
        assertEquals("picture-in-picture", wanted(10).blocker())
        assertEquals("game on the bottom screen", wanted(11).blocker())
        assertEquals("touch editor open", wanted(12).blocker())
        assertEquals("no mouse", wanted(0).blocker())
        assertEquals("multi-window without pointer lock", wanted(2).blocker())
        assertEquals("glasses output without pointer lock", wanted(3).blocker())
        assertNull("a pointer lock keeps capture in multi-window", wanted(1, 2).blocker())
        assertNull("a pointer lock keeps capture with the glasses", wanted(1, 3).blocker())
    }

    @Test fun uncapturedMouseRequestsCaptureAndHoldsMotion() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        val first = guard.uncaptured(10_000, wanted = true, holdable = true)
        assertTrue(first.requestCapture)
        assertTrue(first.holdMotion)
        assertTrue(first.first)
        assertFalse(first.gaveUp)

        val next = guard.uncaptured(10_100, wanted = true, holdable = true)
        assertFalse("requests are 250 ms apart", next.requestCapture)
        assertTrue(next.holdMotion)
        assertFalse(next.first)

        assertTrue(guard.uncaptured(10_250, wanted = true, holdable = true).requestCapture)
        assertTrue(guard.uncaptured(10_999, wanted = true, holdable = true).holdMotion)
        assertEquals(4, guard.held)
    }

    @Test fun motionPassesWhenCaptureDoesNotReturn() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        guard.uncaptured(0, wanted = true, holdable = true)
        val late = guard.uncaptured(1000, wanted = true, holdable = true)
        assertTrue(late.gaveUp)
        assertFalse(late.holdMotion)
        assertFalse("requests slow down once motion passes", late.requestCapture)
        val after = guard.uncaptured(1500, wanted = true, holdable = true)
        assertFalse(after.gaveUp)
        assertFalse(after.holdMotion)
        assertFalse(after.requestCapture)
        assertFalse(guard.uncaptured(1999, wanted = true, holdable = true).requestCapture)
        assertTrue(guard.uncaptured(2000, wanted = true, holdable = true).requestCapture)
        assertFalse(guard.uncaptured(2100, wanted = false, holdable = true).holdMotion)
        assertFalse("closing a panel does not start another hold",
            guard.uncaptured(2200, wanted = true, holdable = true).holdMotion)
    }

    @Test fun aSteadyCaptureStartsOver() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        guard.uncaptured(0, wanted = true, holdable = true)
        assertTrue(guard.uncaptured(1000, wanted = true, holdable = true).gaveUp)
        guard.captureChanged(true, 1200)
        guard.captureChanged(false, 2200)
        val again = guard.uncaptured(2210, wanted = true, holdable = true)
        assertTrue(again.first)
        assertTrue(again.holdMotion)
        assertTrue(again.requestCapture)
        assertTrue(guard.uncaptured(3100, wanted = true, holdable = true).holdMotion)
    }

    @Test fun flappingCaptureStillGivesUp() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        var t = 0L
        var gaveUpAt = -1L
        repeat(8) {
            for (i in 0 until 250 step 10) {
                val v = guard.uncaptured(t + i, wanted = true, holdable = true)
                if (v.gaveUp) gaveUpAt = t + i
            }
            guard.captureChanged(true, t + 250)
            guard.captureChanged(false, t + 300)
            t += 300
        }
        assertTrue("gave up after about 1 s held, at $gaveUpAt", gaveUpAt in 1000L..1300L)
        val later = guard.uncaptured(t, wanted = true, holdable = true)
        assertFalse(later.holdMotion)
        assertFalse(later.first)
    }

    @Test fun heldEventsAreCountedUntilTaken() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        guard.uncaptured(0, wanted = true, holdable = true)
        guard.uncaptured(10, wanted = true, holdable = true)
        guard.captureChanged(true, 20)
        assertEquals(2, guard.takeHeld())
        assertEquals(0, guard.held)
    }

    @Test fun nothingHappensWhileCaptureIsNotWanted() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        val v = guard.uncaptured(0, wanted = false, holdable = true)
        assertFalse(v.requestCapture)
        assertFalse(v.holdMotion)
        assertFalse(v.first)
        assertEquals(0, guard.held)
    }

    @Test fun theHoldTimeAddsUpAcrossUnwantedMoments() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        guard.uncaptured(0, wanted = true, holdable = true)
        guard.uncaptured(600, wanted = true, holdable = true)
        guard.uncaptured(4000, wanted = false, holdable = true)
        val v = guard.uncaptured(5000, wanted = true, holdable = true)
        assertTrue(v.holdMotion)
        assertFalse(v.gaveUp)
        assertFalse("still one stretch for the log", v.first)
        assertTrue(guard.uncaptured(5300, wanted = true, holdable = true).holdMotion)
        assertTrue(guard.uncaptured(5400, wanted = true, holdable = true).gaveUp)
    }

    @Test fun theDesktopSessionRequestsCaptureButKeepsMotion() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        val v = guard.uncaptured(0, wanted = true, holdable = false)
        assertTrue(v.requestCapture)
        assertFalse(v.holdMotion)
        assertTrue(v.first)
        assertFalse(guard.uncaptured(100, wanted = true, holdable = false).holdMotion)
        assertTrue(guard.uncaptured(1000, wanted = true, holdable = false).gaveUp)
        assertFalse(guard.uncaptured(1500, wanted = true, holdable = false).requestCapture)
        assertEquals(0, guard.held)
    }

    @Test fun aRecentRequestFromElsewhereIsNotRepeated() {
        val guard = UncapturedMouseGuard(retryMs = 250, holdMs = 1000, slowRetryMs = 2000)
        guard.requested(1000)
        assertFalse(guard.mayRequest(1100))
        val v = guard.uncaptured(1100, wanted = true, holdable = true)
        assertFalse(v.requestCapture)
        assertTrue(v.holdMotion)
        assertTrue(guard.mayRequest(1250))
    }
}
