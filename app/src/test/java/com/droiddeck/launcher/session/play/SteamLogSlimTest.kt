package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.TimeZone

class SteamLogSlimTest {
    @get:Rule val tmp = TemporaryFolder()
    private val zone = TimeZone.getTimeZone("GMT+11:00")

    @Test fun steamLogsAreCutToTheSessionWindow() {
        val dir = tmp.newFolder("2026-10-28-07-steam")
        // 01:16:39 .. 01:26:41 local.
        File(dir, "events.jsonl").writeText(
            "{\"t\":1793110599268,\"event\":\"session.created\"}\n{\"t\":1793111201730,\"event\":\"session.artifacts_collected\"}\n")
        val steam = File(dir, "steam").apply { mkdirs() }
        File(steam, "gameprocess_log.txt").writeText(listOf(
            "[2026-10-25 06:02:54] Remove 1473480 from running list",
            "[2026-10-28 01:00:00] too early",
            "[2026-10-28 01:07:00] inside the 10 minutes before",
            "  continuation of the line before",
            "[2026-10-28 01:17:32] AppID 1473480 adding PID 24020 as a tracked process",
            "[2026-10-28 01:28:00] inside the 2 minutes after",
            "[2026-10-28 01:40:00] after",
            "  continuation dropped with it",
        ).joinToString("\n", postfix = "\n"))
        File(steam, "compat_log.previous.txt").writeText("old\n".repeat(1000))
        File(steam, "cef_log.txt").writeText((1..100_000).joinToString("\n") { "cef line $it" } + "\n")
        File(steam, "empty.txt").writeText("")

        val freed = SteamLogSlim.slim(dir, zone)
        assertTrue(freed > 0)
        assertEquals(listOf(
            "[2026-10-28 01:07:00] inside the 10 minutes before",
            "  continuation of the line before",
            "[2026-10-28 01:17:32] AppID 1473480 adding PID 24020 as a tracked process",
            "[2026-10-28 01:28:00] inside the 2 minutes after",
        ), File(steam, "gameprocess_log.txt").readLines())
        assertFalse(File(steam, "compat_log.previous.txt").exists())
        val cef = File(steam, "cef_log.txt")
        assertTrue(cef.length() <= SteamLogSlim.TAIL_BYTES)
        assertEquals("cef line 100000", cef.readLines().last())
        assertTrue(cef.readLines().first().startsWith("cef line "))
        assertTrue(File(dir, SteamLogSlim.MARKER).readText().startsWith("slimmed: "))
        // Once.
        assertEquals(0L, SteamLogSlim.slim(dir, zone))
    }

    @Test fun withoutEventsOnlyTheTailsAreKept() {
        val dir = tmp.newFolder("2026-10-28-08-steam")
        val steam = File(dir, "steam").apply { mkdirs() }
        File(steam, "console_log.txt").writeText((1..50_000).joinToString("\n") { "[2026-10-28 01:00:00] line $it" } + "\n")
        SteamLogSlim.slim(dir, zone)
        assertTrue(File(steam, "console_log.txt").length() <= SteamLogSlim.TAIL_BYTES)
    }
}
