package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class XrThreadQosTest {
    @Test fun `the named modes are HyperOS's Mali and render thread levels`() {
        assertEquals(0x26, XrThreadQos.parse("vip")!!.value)
        assertEquals(0x19, XrThreadQos.parse(" RT ")!!.value)
    }

    @Test fun `a level and type, with the RTG bit on request`() {
        assertEquals(0x04, XrThreadQos.parse("4:0")!!.value)
        assertEquals(0x1a, XrThreadQos.parse("10:1")!!.value)
        assertEquals(0x66, XrThreadQos.parse("6:2:rtg")!!.value)
    }

    @Test fun `off, unknown and what libxqos refuses are no mode`() {
        assertNull(XrThreadQos.parse("off"))
        assertNull(XrThreadQos.parse(null))
        assertNull(XrThreadQos.parse(""))
        assertNull(XrThreadQos.parse("0:0"))
        assertNull(XrThreadQos.parse("2:0"))
        assertNull(XrThreadQos.parse("11:1"))
        assertNull(XrThreadQos.parse("6:0"))   // level 6 only as VIP
        assertNull(XrThreadQos.parse("9:0"))   // 7-10 need RT or VIP
        assertNull(XrThreadQos.parse("4:3"))
        assertNull(XrThreadQos.parse("4:0:x"))
    }
}
