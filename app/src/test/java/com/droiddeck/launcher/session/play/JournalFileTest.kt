package com.droiddeck.launcher.session.play

import com.droiddeck.launcher.core.LogRedactor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class JournalFileTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test fun appendsOneLinePerRecord() {
        val j = JournalFile(tmp.newFolder("journal"), LogRedactor::redact)
        j.append(linkedMapOf("type" to "mark", "t" to "2026-10-28T01:37:42+11:00", "note" to "line\nbreak"))
        j.append(linkedMapOf("type" to "play", "id" to "x#1"))
        val lines = j.plays.readLines()
        assertEquals(2, lines.size)
        assertEquals("mark", (Json.parse(lines[0]) as Map<*, *>)["type"])
        assertEquals("line\nbreak", (Json.parse(lines[0]) as Map<*, *>)["note"])
    }

    @Test fun overTheCapTheOldestWholeLinesGo() {
        val j = JournalFile(tmp.newFolder("journal"), { it }, capBytes = 10_000, keepBytes = 6_000)
        for (i in 1..100) j.append(linkedMapOf("type" to "play", "n" to i, "pad" to "x".repeat(90)))
        val lines = j.plays.readLines()
        assertTrue(j.plays.length() <= 10_000)
        assertEquals(100L, (Json.parse(lines.last()) as Map<*, *>)["n"])
        // Whole lines only, in order.
        val ns = lines.map { (Json.parse(it) as Map<*, *>)["n"] as Long }
        assertEquals((ns.first()..100L).toList(), ns)
    }

    @Test fun valuesAreRedactedOneByOne() {
        val j = JournalFile(tmp.newFolder("journal"), LogRedactor::redact)
        val line = j.line(linkedMapOf("first" to listOf("connect failed: password=hunter22 to someone@example.com")))
        assertTrue(line, line.contains("password=<redacted:token>"))
        assertTrue(line, line.contains("<redacted:email>"))
        assertEquals(line, LogRedactor.redact(line))
    }

    @Test fun openStateIsWrittenReadAndCleared() {
        val j = JournalFile(tmp.newFolder("journal"), LogRedactor::redact)
        assertNull(j.readOpen())
        j.writeOpen(linkedMapOf("v" to 1, "appPid" to 27864, "plays" to listOf(linkedMapOf("id" to "a#1"))))
        assertEquals(27864L, j.readOpen()!!.long("appPid"))
        j.writeOpen(null)
        assertNull(j.readOpen())
        j.open.writeText("{broken")
        assertNull(j.readOpen())
    }
}
