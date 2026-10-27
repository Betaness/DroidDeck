package com.droiddeck.launcher.session.play

import com.droiddeck.launcher.session.SessionPaths
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class MirrorRetentionTest {
    @get:Rule val tmp = TemporaryFolder()
    private val now = 1_800_000_000_000L
    private val day = 24L * 3600 * 1000
    private val mb = 1L shl 20

    private fun f(n: Int, bytes: Long = 7 * mb, keep: KeepMarker.Keep? = null, slimmed: Boolean = false) =
        MirrorRetention.Folder("2026-10-%02d-%02d-steam".format(1 + n / 50, n % 50 + 1), bytes, keep, slimmed)

    private fun names(vararg fs: MirrorRetention.Folder) = fs.map { it.name }

    @Test fun newestThreeFullUpToTenSlimmedTheRestGo() {
        val folders = (0 until 14).map { f(it, 80 * mb) }
        val plan = MirrorRetention.plan(folders, now, 100L shl 30, emptySet())
        assertEquals(names(*folders.subList(4, 11).reversed().toTypedArray()), plan.slim)
        assertEquals(names(*folders.subList(0, 4).reversed().toTypedArray()), plan.delete)
    }

    @Test fun flaggedFoldersStayWhileTheirKeepLasts() {
        val crash = KeepMarker.Keep(now + 3 * day, setOf("crash"))
        val expired = KeepMarker.Keep(now - 1, setOf("crash"))
        val folders = listOf(f(0, keep = crash), f(1, keep = expired), f(2, slimmed = true)) + (3 until 13).map { f(it) }
        val plan = MirrorRetention.plan(folders, now, 100L shl 30, emptySet())
        assertTrue(folders[0].name in plan.slim)
        assertTrue(folders[1].name in plan.delete)
        assertTrue(folders[2].name in plan.delete)
        assertFalse(folders[0].name in plan.delete)
    }

    @Test fun overTheCapOldestUnflaggedGoFirstThenFlaggedThenMarked() {
        val marked = KeepMarker.Keep(now + 20 * day, setOf("marked"))
        val crash = KeepMarker.Keep(now + 10 * day, setOf("crash"))
        // 10 folders of 150 MB = 1.5 GB: over the 1 GiB cap.
        val folders = listOf(f(0, 150 * mb, marked), f(1, 150 * mb, crash)) + (2 until 10).map { f(it, 150 * mb, slimmed = true) }
        val plan = MirrorRetention.plan(folders, now, 100L shl 30, emptySet())
        // Unflagged past the newest three go first, oldest first: 2, 3, 4 bring it to 1050 MB, 5 under 1 GiB.
        assertEquals(names(folders[2], folders[3], folders[4], folders[5]), plan.delete)
        // With little free space the cap is 256 MiB: the marked one is the last to go and the newest stays.
        val low = MirrorRetention.plan(folders, now, 1L shl 30, emptySet())
        assertFalse(folders[9].name in low.delete)
        assertTrue(low.delete.indexOf(folders[1].name) < low.delete.indexOf(folders[8].name))
        assertEquals(folders[0].name, low.delete.last())
    }

    @Test fun theRunningSessionIsNeverTouched() {
        val folders = (0 until 12).map { f(it, 200 * mb) }
        val running = folders[0].name
        val plan = MirrorRetention.plan(folders, now, 100L shl 30, setOf(running))
        assertFalse(running in plan.delete)
        assertFalse(running in plan.slim)
    }

    @Test fun keepMarkersOnlyGrow() {
        val dir = tmp.newFolder("2026-10-28-07-steam")
        KeepMarker.extend(dir, 100L, listOf("crash"))
        KeepMarker.extend(dir, 50L, listOf("marked"))
        val k = KeepMarker.read(dir)!!
        assertEquals(100L, k.untilMs)
        assertEquals(setOf("crash", "marked"), k.why)
        assertEquals("until=100 why=crash,marked", File(dir, KeepMarker.NAME).readText().trim())
    }

    @Test fun applySortsChronologicallySoTheHundredthSessionOfADayIsNewest() {
        val base = tmp.newFolder("logs")
        val names = (90..112).map { "2026-10-28-%02d-steam".format(it) }
        for (n in names) File(base, n).mkdirs().also { File(base, "$n/app.log").writeText("x") }
        File(base, "journal-not-a-session").mkdirs()
        MirrorRetention.apply(base, now, 100L shl 30, emptySet(), SessionPaths.chronological, SessionPaths::isSessionFolder)
        val left = base.list()!!.sorted()
        assertTrue("2026-10-28-112-steam" in left)
        assertTrue("2026-10-28-103-steam" in left)
        assertFalse("2026-10-28-99-steam" in left)
        assertFalse("2026-10-28-102-steam" in left)
        assertTrue("journal-not-a-session" in left)
        assertEquals(11, left.size)
    }
}
