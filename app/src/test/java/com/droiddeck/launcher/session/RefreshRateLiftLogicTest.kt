package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class RefreshRateLiftLogicTest {
    /** The setting and the marker through the pairing; [processDied] does what the restorer does. */
    private class FakeSecure(var value: String?) : RefreshRateLiftLogic.Secure {
        var reads = 0
        val writes = ArrayList<String?>()
        var failing = false
        var failingPut = false
        var marker: String? = null
        /** The restorers started, by id; the last one runs, an earlier one stops at its next look. */
        val armed = ArrayList<String>()

        override fun read(): String? {
            check(!failing) { "no pairing" }
            reads++
            return value
        }

        override fun put(value: String) {
            check(!failing && !failingPut) { "no pairing" }
            writes += value
            this.value = value
        }

        override fun putBack(original: String?) {
            check(!failing) { "no pairing" }
            writes += original
            value = original
            marker = null
        }

        override fun marker(): RefreshRateLiftLogic.Marker? {
            check(!failing) { "no pairing" }
            return RefreshRateLiftLogic.parseMarker(marker ?: "")
        }

        override fun arm(record: Record) {
            check(!failing) { "no pairing" }
            marker = RefreshRateLiftLogic.markerLine(record)
            armed += record.id
        }

        override fun disarm() {
            check(!failing) { "no pairing" }
            marker = null
        }

        /** The app's process is gone: its restorer, when its marker still names it, acts as [RefreshRateLiftLogic.armCommand]'s does. */
        fun processDied() {
            val running = armed.lastOrNull() ?: return
            armed.clear()
            val m = RefreshRateLiftLogic.parseMarker(marker ?: "") ?: return
            if (m.id != running || m.outcome != null) return
            val line = "${m.id} ${m.original ?: "unset"} ${m.written}"
            marker = if (value == m.written) {
                value = m.original
                writes += m.original
                "$line ${RefreshRateLiftLogic.RESTORED}"
            } else "$line ${RefreshRateLiftLogic.LEFT}"
        }
    }

    private class FakeRecords : RefreshRateLiftLogic.Records {
        var record: Record? = null
        override fun load() = record
        override fun save(record: Record) { this.record = record }
        override fun clear() { record = null }
    }

    private val records = FakeRecords()
    private var ids = 0

    private fun logic(secure: FakeSecure) = RefreshRateLiftLogic(secure, records) { "r${++ids}" }

    @Test fun `raised while shown and put back after, with the value before recorded and a restorer started first`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 120 -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144", "r1", confirmed = true), records.record)
        assertEquals("r1 120 144", secure.marker)
        assertEquals(listOf("r1"), secure.armed)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144", "120"), secure.writes)
    }

    @Test fun `a value the owner changed meanwhile is left, and the record and the restorer dropped`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        secure.value = "90"
        assertEquals("user_refresh_rate 90, changed from 144 meanwhile: left as it is", lift.hidden())
        assertEquals("90", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `a raised value left by a process that died is put back at the next app or session start`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear() // the restorer did not run (the device restarted)

        val afterCrash = logic(secure)
        assertEquals("user_refresh_rate 144 -> 120 again", afterCrash.appStarted())
        assertEquals("120", secure.value)
        assertNull(records.record)
        assertNull(secure.marker)

        logic(secure).shown(true, 144)
        secure.armed.clear()
        assertEquals("user_refresh_rate 144 -> 120 again", logic(secure).sessionStarted(true, 144))
        assertEquals("120", secure.value)
        assertNull(records.record)
    }

    @Test fun `the restorer puts the value back when the process dies, and the next process only drops its record`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.processDied()
        assertEquals("120", secure.value)
        assertEquals("r1 120 144 restored", secure.marker)

        assertEquals("user_refresh_rate 120, changed from 144 meanwhile: left as it is", logic(secure).appStarted())
        assertNull(records.record)
        assertNull(secure.marker)
        assertEquals(listOf<String?>("144", "120"), secure.writes)
    }

    @Test fun `the owner's own 144 after the restorer acted is not overwritten`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.processDied()
        secure.value = "144"

        val next = logic(secure)
        assertEquals("user_refresh_rate 144, put back by the restorer and set to 144 again since: left as it is", next.appStarted())
        assertEquals("144", secure.value)
        assertNull(records.record)

        // At a session instead of an app start, after the restorer found it changed: nothing raised, nothing recorded.
        records.record = Record("120", "144", "r2")
        secure.marker = "r2 120 144 left"
        assertEquals("user_refresh_rate 144, changed and set to 144 again since: already reaches the panel's 144 Hz", logic(secure).shown(true, 144))
        assertNull(records.record)
        assertEquals("144", secure.value)
    }

    @Test fun `a record whose write was never confirmed by a process that died is dropped without a write`() {
        val secure = FakeSecure("144")
        records.record = Record("120", "144", "r9", confirmed = false)
        assertEquals("user_refresh_rate 144, the write of 144 was not confirmed: left as it is", logic(secure).appStarted())
        assertEquals("144", secure.value)
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
    }

    @Test fun `a write that failed in this process is still put back`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        secure.failingPut = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144) }
        assertEquals(Record("120", "144", "r1", confirmed = false), records.record)
        assertEquals("r1 120 144", secure.marker)
        // The write went through after all (the answer was lost).
        secure.value = "144"
        secure.failingPut = false
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
        assertNull(records.record)
    }

    @Test fun `a record lost with the app's data is taken from the restorer's marker`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear() // the restorer did not run (the device restarted)
        records.record = null // the app's data cleared

        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 recorded only in the restorer's marker: kept, the restorer now watches this process", lift.shown(true, 144))
        assertEquals(Record("120", "144", "r2"), records.record)
        assertEquals("r2 120 144", secure.marker)
        assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
        assertEquals("120", secure.value)
    }

    @Test fun `a new process keeps a value raised before and starts its own restorer`() {
        val secure = FakeSecure("120")
        logic(secure).shown(true, 144)
        secure.armed.clear()
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144 raised before: kept, the restorer now watches this process", lift.shown(true, 144))
        assertEquals(Record("120", "144", "r2"), records.record)
        assertEquals("r2 120 144", secure.marker)
        assertNull(lift.shown(true, 144))
        assertEquals(listOf<String?>("144"), secure.writes)
    }

    @Test fun `nothing is put back at app or session start while a session activity is shown`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        lift.shown(true, 144)
        assertNull(lift.appStarted())
        assertNull(lift.sessionStarted(true, 144))
        assertEquals("144", secure.value)
        assertEquals(Record("120", "144", "r1"), records.record)
        assertNull(lift.retry(true))
        assertFalse(lift.pending(true))
        assertTrue(lift.pending(false))
    }

    @Test fun `an unset value is recorded as unset and deleted again`() {
        val secure = FakeSecure(null)
        val lift = logic(secure)
        assertEquals("user_refresh_rate unset -> 144 while a session is on screen", lift.shown(true, 144))
        assertEquals(Record(null, "144", "r1"), records.record)
        assertEquals("r1 unset 144", secure.marker)
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
        assertTrue(secure.armed.isEmpty())
    }

    @Test fun `a value that reaches the top rate already is left alone`() {
        val secure = FakeSecure("144")
        val lift = logic(secure)
        assertEquals("user_refresh_rate 144: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
        assertNull(lift.hidden())
        assertEquals("144", secure.value)
        assertTrue(secure.writes.isEmpty())
        assertTrue(secure.armed.isEmpty())
        assertFalse(RefreshRateLiftLogic.below("144.0", 144))
        assertFalse(RefreshRateLiftLogic.below("165", 144))
        assertFalse(RefreshRateLiftLogic.below("120", 0))
    }

    @Test fun `repeated starts and stops keep the first value`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        repeat(3) {
            lift.shown(true, 144)
            val reads = secure.reads
            assertNull(lift.shown(true, 144))
            assertNull(lift.sessionStarted(true, 144))
            assertEquals(reads + 2, secure.reads)
            assertEquals("120", records.record?.original)
            assertEquals("user_refresh_rate 144 -> 120 again", lift.hidden())
            assertNull(lift.hidden())
            assertNull(lift.sessionEnded())
            assertEquals("120", secure.value)
        }
        assertEquals(listOf<String?>("144", "120", "144", "120", "144", "120"), secure.writes)
        assertEquals(listOf("r1", "r2", "r3"), secure.armed)
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
        assertEquals(Record("60", "144", "r2"), records.record)
        lift.hidden()
        assertEquals("60", secure.value)

        lift.shown(true, 144)
        secure.value = "144.0"
        assertNull(lift.shown(true, 144))
        secure.value = "165"
        assertEquals("user_refresh_rate 165, changed from 144 meanwhile: already reaches the panel's 144 Hz", lift.shown(true, 144))
        assertNull(records.record)
        assertNull(secure.marker)
    }

    @Test fun `a failing pairing changes nothing, and a record left is put back by a retry`() {
        val secure = FakeSecure("120")
        val lift = logic(secure)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.shown(true, 144) }
        assertNull(records.record)
        assertEquals("120", secure.value)
        assertFalse(lift.pending(true))

        secure.failing = false
        lift.shown(true, 144)
        secure.failing = true
        assertThrows(IllegalStateException::class.java) { lift.hidden() }
        assertEquals(Record("120", "144", "r1"), records.record)
        assertFalse(lift.visible)
        assertTrue(lift.pending(true))
        assertThrows(IllegalStateException::class.java) { lift.retry(true) }
        secure.failing = false
        assertEquals("user_refresh_rate 144 -> 120 again", lift.retry(true))
        assertEquals("120", secure.value)
        assertFalse(lift.pending(true))
    }

    @Test fun `a restorer that could not be started leaves the setting and no record`() {
        val secure = object : RefreshRateLiftLogic.Secure by FakeSecure("120") {
            override fun arm(record: Record) = throw IllegalStateException("/data/local/tmp not writable")
        }
        assertThrows(IllegalStateException::class.java) { RefreshRateLiftLogic(secure, records) { "r1" }.shown(true, 144) }
        assertEquals("120", secure.read())
        assertNull(records.record)
    }

    @Test fun `a record that is not a rate is dropped without a write`() {
        val secure = FakeSecure("144")
        records.record = Record("120; reboot", "144", "r1")
        assertEquals("record of user_refresh_rate not understood (120; reboot -> 144): dropped", logic(secure).appStarted())
        assertNull(records.record)
        assertTrue(secure.writes.isEmpty())
        records.record = Record("120", "144", "r1; reboot")
        logic(secure).appStarted()
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

    @Test fun `the marker is read back as written, and anything else as none`() {
        val record = Record(null, "144", "a1b2")
        assertEquals(RefreshRateLiftLogic.Marker("a1b2", null, "144", null), RefreshRateLiftLogic.parseMarker(RefreshRateLiftLogic.markerLine(record) + "\n"))
        assertEquals(RefreshRateLiftLogic.Marker("a1b2", "120", "144", "restored"), RefreshRateLiftLogic.parseMarker("a1b2 120 144 restored"))
        assertNull(RefreshRateLiftLogic.parseMarker(""))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120"))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120 144 maybe"))
        assertNull(RefreshRateLiftLogic.parseMarker("A1;B2 120 144"))
        assertNull(RefreshRateLiftLogic.parseMarker("a1b2 120 144 restored extra"))
        assertEquals("/data/local/tmp/refresh-lift.com.droiddeck.launcher.g2", RefreshRateLiftLogic.markerPath("com.droiddeck.launcher.g2"))
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.markerPath("a b") }
        assertThrows(IllegalArgumentException::class.java) { RefreshRateLiftLogic.armCommand("/tmp/m", 1, Record("120", "144", "x'y")) }
    }
}
