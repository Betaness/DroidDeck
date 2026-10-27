package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class JsonTest {
    @Test fun writesInInsertionOrderAndReadsBack() {
        val m = linkedMapOf<String, Any?>(
            "z" to 1, "a" to "x\"y\\z\n\u0001", "n" to null, "b" to true, "d" to 78.4, "w" to 120.0,
            "l" to listOf(1, null, 2.5), "o" to linkedMapOf("k" to "v"), "ia" to intArrayOf(1, 2),
        )
        val text = Json.write(m)
        assertEquals("""{"z":1,"a":"x\"y\\z\n\u0001","n":null,"b":true,"d":78.4,"w":120,"l":[1,null,2.5],"o":{"k":"v"},"ia":[1,2]}""", text)
        val back = Json.parse(text) as Map<*, *>
        assertEquals(listOf("z", "a", "n", "b", "d", "w", "l", "o", "ia"), back.keys.toList())
        assertEquals("x\"y\\z\n\u0001", back["a"])
        assertEquals(1L, back["z"])
        assertEquals(78.4, back["d"])
        assertEquals(listOf(1L, null, 2.5), back["l"])
        assertEquals(Json.write(m), Json.write(back))
    }

    @Test fun nanIsNull() {
        assertEquals("[null,null]", Json.write(listOf(Double.NaN, Float.POSITIVE_INFINITY)))
    }

    @Test fun malformedTextIsRefused() {
        assertNull(Json.parseOrNull("""{"a":1"""))
        assertNull(Json.parseOrNull("""{"a":1} x"""))
        assertNull(Json.parseOrNull(""))
        assertEquals("é", Json.parse("\"\\u00e9\""))
    }
}
