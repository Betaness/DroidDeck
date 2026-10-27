package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ExitClassifierTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun findings(vararg lines: String, exit: String? = null): LogSliceScanner.Findings {
        val f = tmp.newFile().apply {
            writeText((lines.toList() + listOfNotNull(
                exit?.let { "[2026-10-28 01:17:32] AppID 7 adding PID 5 as a tracked process \"x\"" },
                exit?.let { "[2026-10-28 01:20:00] AppID 7 no longer tracking PID 5, exit code $it" },
            )).joinToString("\n", postfix = "\n"))
        }
        return LogSliceScanner().scan(listOf(LogSliceScanner.Source("log", f)), appid = 7)
    }

    private fun ev(
        durS: Long = 600, f: LogSliceScanner.Findings? = findings(), frames: Long = 30_000,
        appKilled: ExitClassifier.AppExit? = null, stopped: Boolean = false, vortek: Int? = null, vortekToEnd: Long? = null,
        usedVortek: Boolean = false, zeroRun: Long = 0, stall: Double? = null, mean: Double? = 60.0, low1: Double? = 30.0,
        at60Thermal: Double? = 0.0, marked: Boolean = false,
    ) = ExitClassifier.Evidence(
        durS = durS, appKilled = appKilled, sessionStopped = stopped, sessionStatus = if (stopped) 0 else null,
        vortekExit = vortek, vortekToEndS = vortekToEnd, usedVortek = usedVortek, findings = f, steamExitCode = f?.steamExitCode,
        gameFrames = frames, longestZeroRunS = zeroRun, longestStallS = stall, meanFpsAfterWarmup = mean, low1 = low1,
        at60Thermal = at60Thermal, marked = marked,
    )

    @Test fun reasonsInOrder() {
        val killed = ExitClassifier.AppExit("LOW_MEMORY", 0, "lmk", 1L)
        assertEquals("app_killed", ExitClassifier.classify(ev(appKilled = killed, f = findings("Crash!!!"))).reason)
        assertEquals("session_stopped", ExitClassifier.classify(ev(stopped = true)).reason)
        assertEquals("vortek_exit", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 3)).reason)
        assertEquals("vortek_exit", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 300, usedVortek = true)).reason)
        // The server died long before a hybris game ended: not why it ended.
        assertEquals("normal", ExitClassifier.classify(ev(vortek = 139, vortekToEnd = 300)).reason)
        assertEquals("device_lost", ExitClassifier.classify(ev(f = findings("err:   DxvkDevice: VK_ERROR_DEVICE_LOST"))).reason)
        assertEquals("normal", ExitClassifier.classify(ev()).reason)
    }

    @Test fun unityCrashWithExitCodeZeroIsACrash() {
        val exit = ExitClassifier.classify(ev(f = findings("Crash!!!", exit = "0")))
        assertEquals("crash", exit.reason)
        assertEquals(0, exit.steamCode)
        assertEquals("log: Crash!!!", exit.detail)
        assertTrue("crash" in exit.flags)
    }

    @Test fun nonZeroExitCodeIsACrashWithItsSignal() {
        val exit = ExitClassifier.classify(ev(f = findings(exit = "139")))
        assertEquals("crash", exit.reason)
        assertEquals(11, exit.signal)
        assertEquals("normal", ExitClassifier.classify(ev(f = findings(exit = "0"))).reason)
    }

    @Test fun shortPlaysWithoutAFrameOrWithMissingFeaturesFailedToLaunch() {
        assertEquals("launch_failed", ExitClassifier.classify(ev(durS = 20, frames = 0)).reason)
        assertEquals("normal", ExitClassifier.classify(ev(durS = 20, frames = 100)).reason)
        val unsupported = ExitClassifier.classify(ev(f = findings("err:   DxvkAdapter: Skipping: Device does not support required feature 'x'")))
        assertEquals("launch_failed", unsupported.reason)
        assertTrue(unsupported.detail!!.contains("fillMode") || unsupported.detail!!.contains("Skipping"))
    }

    @Test fun flagsAreIndependent() {
        val exit = ExitClassifier.classify(ev(stopped = true, zeroRun = 45, mean = 14.0, at60Thermal = 0.5, marked = true,
            f = findings("Scudo ERROR: out of memory", "hybris-x11: window 0x1 1704x1136 now hidden, frames paced (t=1)")))
        assertEquals("session_stopped", exit.reason)
        assertEquals(listOf("freeze", "low_fps", "oom", "hidden_paced", "at60_thermal", "marked"), exit.flags)
        assertEquals(listOf("freeze"), ExitClassifier.classify(ev(stall = 40.0)).flags)
        // A loading screen that presents nothing for a while is not one.
        assertEquals(emptyList<String>(), ExitClassifier.classify(ev(stall = 12.0)).flags)
        assertEquals(listOf("low_fps"), ExitClassifier.classify(ev(low1 = 5.0)).flags)
    }

    @Test fun keepDaysFollowTheFlags() {
        val day = 24L * 3600 * 1000
        assertEquals(null, ExitClassifier.keepFor(listOf("hidden_paced", "at60_thermal")))
        assertEquals(7 * day, ExitClassifier.keepFor(listOf("low_fps"))!!.first)
        assertEquals(14 * day, ExitClassifier.keepFor(listOf("low_fps", "crash"))!!.first)
        assertEquals(30 * day to listOf("crash", "marked"), ExitClassifier.keepFor(listOf("crash", "marked")))
    }
}
