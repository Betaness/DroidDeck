package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Test

class HybrisGpuTest {
    @Test fun `unset is in-process on the O3 only, and a set path is kept`() {
        assertEquals("hybris", HybrisGpu.path(null, onO3 = true))
        assertEquals("vortek", HybrisGpu.path(null, onO3 = false))
        assertEquals("hybris", HybrisGpu.path(" ", onO3 = true))
        assertEquals("vortek", HybrisGpu.path("vortek", onO3 = true))
        assertEquals("hybris", HybrisGpu.path("hybris", onO3 = false))
    }
}
