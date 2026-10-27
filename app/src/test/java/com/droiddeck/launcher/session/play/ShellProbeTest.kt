package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ShellProbeTest {
    // dumpsys thermalservice on the Xring O3 (trimmed), and the settings line.
    private val sample = """
        Cached temperatures:
        	Temperature{mValue=35.0, mType=-1, mName=shell-thermal, mStatus=0}
        	Temperature{mValue=59.875, mType=-1, mName=gpu-ss-max, mStatus=0}
        	Temperature{mValue=46.125, mType=1, mName=gpu-ss-1, mStatus=0}
        	Temperature{mValue=60.0, mType=-1, mName=soc-hp, mStatus=0}
        	Temperature{mValue=35.395, mType=3, mName=quiet_therm, mStatus=0}
        	Temperature{mValue=34.3, mType=-1, mName=battery, mStatus=0}
        	Temperature{mValue=51.625, mType=0, mName=cpu-0, mStatus=0}
        	Temperature{mValue=20.0, mType=13, mName=soc-lowtemp, mStatus=0}
        	Temperature{mValue=NaN, mType=0, mName=cpu-broken, mStatus=0}
        Current temperatures from HAL:
        	Temperature{mValue=55.25, mType=0, mName=cpu-0, mStatus=0}
        	Temperature{mValue=59.75, mType=0, mName=cpu-1, mStatus=0}
        LIMIT 60
    """.trimIndent()

    @Test fun parsesSkinSocBatteryAndTheRefreshCap() {
        val r = ShellProbe.parse(sample, 5L)
        assertEquals(35.395f, r.skinC!!, 0.001f)
        assertEquals(59.75f, r.socC!!, 0.001f) // the current cpu-1 reading
        assertEquals(34.3f, r.batteryC!!, 0.001f)
        assertEquals(60, r.limitHz)
    }

    @Test fun nothingReadableIsNull() {
        val r = ShellProbe.parse("LIMIT null\n", 5L)
        assertNull(r.skinC)
        assertNull(r.socC)
        assertNull(r.limitHz)
    }

    @Test fun runsOnlyWhileAPlayIsOpenAndEveryThirtySeconds() {
        ShellProbe.reset()
        var runs = 0
        val shell: (String) -> String = { runs++; sample }
        ShellProbe.wanted = false
        ShellProbe.tick(100_000, shell)
        assertEquals(0, runs)
        ShellProbe.wanted = true
        ShellProbe.tick(100_000, shell)
        ShellProbe.tick(110_000, shell)
        assertEquals(1, runs)
        ShellProbe.tick(131_000, shell)
        assertEquals(2, runs)
        assertEquals(60, ShellProbe.last!!.limitHz)
        ShellProbe.wanted = false
        ShellProbe.reset()
    }
}
