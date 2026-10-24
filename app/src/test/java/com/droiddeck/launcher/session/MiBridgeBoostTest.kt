package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.MiBridgeBoost.Hold
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class MiBridgeBoostTest {
    @Test fun `holds are read from the setting`() {
        assertEquals(listOf(Hold("gpu", 1)), MiBridgeBoost.parse("gpu=1"))
        assertEquals(listOf(Hold("gpu", 3), Hold("cpu", 1), Hold("ddr", 2)), MiBridgeBoost.parse(" GPU=3, cpu=1 ,ddr=2"))
        assertEquals(10_000, Hold("cpu", 1).timeoutMs)
        assertEquals(1_500, Hold("gpu", 3).timeoutMs)
        assertEquals("Ddr", Hold("ddr", 2).method)
    }

    @Test fun `a malformed setting holds nothing`() {
        assertNull(MiBridgeBoost.parse(""))
        assertNull(MiBridgeBoost.parse("gpu"))
        assertNull(MiBridgeBoost.parse("gpu=4"))
        assertNull(MiBridgeBoost.parse("gpu=0"))
        assertNull(MiBridgeBoost.parse("npu=1"))
        assertNull(MiBridgeBoost.parse("gpu=1,gpu=2"))
    }

    @Test fun `the package goes into the list once and comes out alone`() {
        val g2 = "com.droiddeck.launcher.g2"
        assertEquals("com.miui.home,$g2", MiBridgeBoost.withPackage("com.miui.home", g2))
        assertEquals(g2, MiBridgeBoost.withPackage("", g2))
        assertNull(MiBridgeBoost.withPackage("a.b,$g2", g2))
        assertEquals("com.miui.home,a.b", MiBridgeBoost.withoutPackage("com.miui.home,$g2,a.b", g2))
        assertEquals("", MiBridgeBoost.withoutPackage(g2, g2))
        assertNull(MiBridgeBoost.withoutPackage("com.miui.home", g2))
    }
}
