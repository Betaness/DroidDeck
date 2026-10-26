package com.droiddeck.launcher.session

import org.junit.Assert.assertEquals
import org.junit.Test

class HybrisGpuTest {
    @Test fun `unset or blank is Vortek, and a set path is kept`() {
        assertEquals("vortek", HybrisGpu.path(null))
        assertEquals("vortek", HybrisGpu.path(" "))
        assertEquals("hybris", HybrisGpu.path("hybris"))
        assertEquals("vortek", HybrisGpu.path("vortek"))
    }
}
