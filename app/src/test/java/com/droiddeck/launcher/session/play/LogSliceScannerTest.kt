package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class LogSliceScannerTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun file(name: String, vararg lines: String) = tmp.newFile(name).apply { writeText(lines.joinToString("\n", postfix = "\n")) }

    @Test fun findsProblemsAndFactsInRealLines() {
        val session = file("session.log",
            "vortek-x11: swapchain 32x32 for window 0x2c0002f: present mode 2, 3 images",
            "vortek-x11: window 0x2c00034 32x32 now hidden, frames paced (t=17850.390)",
            "droiddeck-game-env: Vulkan in-process (libhybris), Steam stays on Vortek",
            "info:  DXVK: v3.1.1-97-ga78a467cadb22be",
            "hybris-wsi: swapchain 1704x1136 format 44, 3 images, window 0x3a00002, present mode 2",
            "warn:  D3D11: something harmless",
            "err:   DxvkAdapter: Skipping: Device does not support required feature 'fillModeNonSolid'",
            "hybris-x11: window 0x3a00002 1704x1136 now hidden, frames paced (t=1.0)",
            "hybris-x11: 1200 frames, 58.5 fps (zero-copy)",
        )
        val app = file("app.log",
            "10-28 01:16:43.893 27864 29770 I DroidDeckWayland: [stats] last 10 s: 600 frames on screen (60.0 fps) | 590 GPU frames from games | 10 window redraws | 1 windows open | 0 Vortek zero-copy frames",
            "10-28 01:16:42.990 27864 29770 I DroidDeckWayland: [vortek] zero-copy swapchain 19 of presenter 1: 3408x2272, 3 AHardwareBuffers",
            "10-28 01:19:50.493 27864 21768 I GameThreadHints: cluster steering: moved 3 processes to cores 0-3: cores 4-7 held",
            "10-28 01:20:10.392 32745 11201 I VortekServer: vortek-server exited: 139",
        )
        val player = file("Player.log",
            "    Renderer: Mali-G2-Ultra-NX MC16 (ID=0x8001)",
            "Crash!!!",
            "Crash!!!",
        )
        val steam = file("gameprocess_log.txt",
            "[2026-10-28 01:17:32] AppID 3167020 adding PID 9246 as a tracked process \"/root/.local/share/Steam/linuxarm64/steam-launch-wrapper ...\"",
            "[2026-10-28 01:17:32] AppID 3167020 adding PID 9248 as a tracked process",
            "[2026-10-28 01:26:14] AppID 3167020 no longer tracking PID 9248, exit code -1",
            "[2026-10-28 01:26:15] AppID 3167020 no longer tracking PID 9246, exit code 0",
        )
        val f = LogSliceScanner().scan(listOf(
            LogSliceScanner.Source("session.log", session), LogSliceScanner.Source("app.log", app),
            LogSliceScanner.Source("Player.log", player), LogSliceScanner.Source("gameprocess_log.txt", steam),
            LogSliceScanner.Source("missing", File(tmp.root, "nope")),
        ), appid = 3167020)
        assertEquals(listOf("session.log", "app.log", "Player.log", "gameprocess_log.txt"), f.sources)
        assertEquals("hybris", f.gpuUsed)
        assertEquals("v3.1.1-97-ga78a467cadb22be", f.dxvk)
        assertEquals("Mali-G2-Ultra-NX MC16 (ID=0x8001)", f.renderer)
        assertEquals(listOf("3408x2272", "1704x1136"), f.swapchains())
        assertEquals(listOf(600), f.screenFrames)
        assertEquals(listOf(590), f.gpuFrames)
        assertEquals(listOf(58.5), f.x11Fps)
        assertEquals(1, f.steers)
        assertEquals(139, f.vortekExit)
        assertEquals(9246, f.steamWrapperPid)
        assertEquals(0, f.steamExitCode)
        assertEquals("2026-10-28 01:26:15", f.steamEndedAt)
        assertEquals(2, f.errors.getValue("crash").n)
        assertEquals("Player.log: Crash!!!", f.firstLine("crash"))
        assertTrue(f.has("unsupported"))
        // The 32x32 helper window hidden is routine; the game-sized one is not.
        assertEquals(1, f.errors.getValue("hidden").n)
        assertFalse(f.has("device_lost"))
    }

    @Test fun readsFromTheOffsetAndResyncsToALineStart() {
        val log = file("session.log", "err:vulkan:before the play", "DEVICE_LOST in the play")
        val mid = "err:vulkan:bef".length.toLong()
        val f = LogSliceScanner().scan(listOf(LogSliceScanner.Source("session.log", log, mid)))
        assertFalse(f.has("vulkan"))
        assertTrue(f.has("device_lost"))
        val atStart = LogSliceScanner().scan(listOf(LogSliceScanner.Source("session.log", log, "err:vulkan:before the play\n".length.toLong())))
        assertFalse(atStart.has("vulkan"))
        assertTrue(atStart.has("device_lost"))
        val past = LogSliceScanner().scan(listOf(LogSliceScanner.Source("session.log", log, 10_000)))
        assertTrue(past.errors.isEmpty())
    }

    @Test fun keepsAFewLinesPerKindAndCutsLongOnes() {
        val lines = (1..50).map { "err:vulkan:line $it " + "x".repeat(400) } + (1..50).map { "err:module:line $it" }
        val f = LogSliceScanner(perKind = 3, total = 5, lineMax = 240).scan(listOf(LogSliceScanner.Source("s", file("s.log", *lines.toTypedArray()))))
        assertEquals(50, f.errors.getValue("vulkan").n)
        assertEquals(3, f.errors.getValue("vulkan").first.size)
        assertEquals(240, f.errors.getValue("vulkan").first[0].length)
        assertEquals(2, f.errors.getValue("module").first.size)
        assertEquals(50, f.errors.getValue("module").n)
    }

    @Test fun readsOnlyTheLastPartOfAHugeSlice() {
        val log = tmp.newFile("big.log")
        log.writeText("Crash!!! early\n" + "filler line\n".repeat(2000) + "DEVICE_LOST late\n")
        val f = LogSliceScanner(maxBytes = 1000).scan(listOf(LogSliceScanner.Source("big", log)))
        assertFalse(f.has("crash"))
        assertTrue(f.has("device_lost"))
    }

    @Test fun nothingFoundIsEmpty() {
        val f = LogSliceScanner().scan(listOf(LogSliceScanner.Source("s", file("s.log", "all fine"))))
        assertTrue(f.errors.isEmpty())
        assertNull(f.steamExitCode)
    }
}
