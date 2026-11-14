package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RefreshRateLiftLogicTest {
    private class FakeSecure(var value: String?) : RefreshRateLiftLogic.Secure {
        var reads = 0
        val writes = ArrayList<String?>()
        var failing = false

        override fun read(): String? {
            check(!failing) { "no pairing" }
            reads++
            return value
        }

        override fun put(value: String) {
            check(!failing) { "no pairing" }
            writes += value
            this.value = value
        }

        override fun delete() {
            check(!failing) { "no pairing" }
            writes += null
            value = null
        }
    }

    private class FakeRecords : RefreshRateLiftLogic.Records {
        var record: Record? = null
        override fun load() = record
        override fun save(record: Record) { this.record = record }
        override fun clear() { record = null }
    }

    private val records = FakeRecords()

    private fun logic(secure: FakeSecure) = RefreshRateLiftLogic(secure, records)

    @Test fun `raised while shown and put back after, with the value before recorded first`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144"), records.record)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertEquals(listOf<String?>("144", "120"), secure.writes)
    }

    @Test fun `a value the owner changed meanwhile is left, and the record dropped`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "90"
        assertEquals("user_refresh_rate is 90, not the 144 written here: left as it is", lift.hidden())
        assertEquals("90", secure.value)
        assertNull(records.record)
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `a raised value left by a process that died is put back at the next app or session start`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)

        val afterCrash = logic(secure)
        assertEquals("user_refresh_rate 144 -> 120 again", afterCrash.appStarted())
        assertEquals("120", secure.value)
        assertNull(records.record)

        logic(secure).shown(true, 144)
        assertEquals("user_refresh_rate 144 -> 120 again", logic(secure).sessionStarted(true, 144))
        assertEquals("120", secure.value)
        assertNull(records.record)
    }

    @Test fun `nothing is put back at app or session start while a session activity is shown`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertNull(lift.appStarted())
        assertNull(lift.sessionStarted(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144"), records.record)
    }

    @Test fun `an unset value is recorded as unset and deleted again`() {
        val secure = FakeSecure(null)
        val lift = logic(secure)
        assertEquals("user_refresh_rate unset -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals(Record(null, "144"), records.record)
        assertEquals("user_refresh_rate 144 -> unset again", lift.hidden())
        assertNull(secure.value)
        assertNull(records.record)
        assertEquals(listOf<String?>("144", null), secure.writes)
    }

    @Test fun `turning the setting off puts the value back at once`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.turnedOff())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertTrue(lift.visible)
    }

    @Test fun `with the setting off nothing is read or written`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertNull(lift.shown(false, 144))
        assertNull(lift.hidden())
        assertNull(lift.turnedOff())
        assertEquals(0, secure.reads)
        assertTrue(secure.writes.isEmpty())
    }

    @Test fun `a value that reaches the top rate already is left alone`() {
        val secure = FakeSecure("144")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
        assertNull(lift.hidden())
        assertEquals("144", secure.value)
        assertTrue(secure.writes.isEmpty())
        assertFalse(RefreshRateLiftLogic.below("144.0", 144))
        assertFalse(RefreshRateLiftLogic.below("165", 144))
        assertFalse(RefreshRateLiftLogic.below("120", 0))
    }

    @Test fun `repeated starts and stops keep the first value`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        repeat(3) {
            lift.shown(true, 144)
            assertNull(lift.shown(true, 144))
            assertNull(lift.sessionStarted(true, 144))
            assertEquals(Record("120", "144"), records.record)
            assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
            assertNull(lift.hidden())
            assertNull(lift.sessionEnded())
            assertEquals("120", secure.value)
        }
        assertEquals(listOf<String?>("144", "120", "144", "120", "144", "120"), secure.writes)
    }

    @Test fun `the session end puts it back, and a new session in a shown activity raises it again`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.sessionEnded())
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.sessionStarted(true, 144))
        assertEquals("144", secure.value)
    }

    @Test fun `a value changed by the owner while shown starts a new record`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "60"
        assertEquals("user_refresh_rate 60, changed from 144 meanwhile -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals(Record("60", "144"), records.record)
        lift.hidden()
        assertEquals("60", secure.value)

        lift.shown(true, 144)
        secure.value = "144.0"
        assertNull(lift.shown(true, 144))
        secure.value = "165"
        assertEquals("user_refresh_rate 165, changed from 144 meanwhile: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
    }

    @Test fun `a failing pairing changes nothing, and a record left is put back later`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144) }
        assertNull(records.record)
        assertEquals("120", secure.value)

        secure.failing = false
        lift.shown(true, 144)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.hidden() }
        assertEquals(Record("120", "144"), records.record)
        assertFalse(lift.visible)
        secure.failing = false
        assertEquals("user_refresh_rate 144 -> 120 again", lift.appStarted())
        assertEquals("120", secure.value)
    }

    @Test fun `a record that is not a rate is dropped without a write`() {
        val secure = FakeSecure("144")
        records.record = Record("120; reboot", "144")
        assertEquals("record of user_refresh_rate not understood (120; reboot -> 144): dropped", logic(secure).appStarted())
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
    }

    @Test fun `the shell's answer is read as a rate or unset`() {
        assertNull(RefreshRateLiftLogic.parse("null\n"))
        assertEquals("120", RefreshRateLiftLogic.parse("120\n"))
        assertEquals("144.0", RefreshRateLiftLogic.parse(" 144.0 "))
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("auto") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.parse("120\n60") }
        assertTrue(RefreshRateLiftLogic.same("144", "144.0"))
        assertFalse(RefreshRateLiftLogic.same("144", null))
        assertTrue(RefreshRateLiftLogic.same(null, null))
        assertTrue(RefreshRateLiftLogic.below(null, 144))
    }
}
