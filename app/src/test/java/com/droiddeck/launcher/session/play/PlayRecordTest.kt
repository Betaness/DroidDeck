package com.droiddeck.launcher.session.play

import com.droiddeck.launcher.core.LogRedactor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.util.TimeZone

class PlayRecordTest {
    @get:Rule val tmp = TemporaryFolder()
    private val zone = TimeZone.getTimeZone("GMT+11:00")
    private val t0 = 1_793_110_652_000L

    /** A play as the journal holds it after half an hour of Duckov on hybris. */
    fun play(): PlayState {
        val s = PlayState("2026-10-28-07-steam#1", "2026-10-28-07-steam", 3167020, "Escape from Duckov",
            "Escape from Duckov/Duckov.exe", "steam", t0)
        s.app = linkedMapOf("version" to "0.6.0 (17)", "commit" to "e18add60c1a2", "ci" to true)
        s.gpuAsked = "hybris"
        s.env = linkedMapOf("BL_SYNC" to "pool", "DROIDDECK_GPU_PATH" to "hybris", "LD_PRELOAD" to "libhybris-preload.so,libfakeinput.so",
            "PROTON_NO_NTSYNC" to "1", "VK_ICD_FILENAMES" to "hybris_icd.aarch64.json")
        s.settings = linkedMapOf("mibridge" to "mibridge: asking for cpu level 1, gpu level 1, ddr level 1 (see the MiBridgeBoost lines for the outcome)",
            "joyose" to "joyose: stopping it during the session", "regulator" to "CLEAR", "joyoseWatch" to "stopping",
            "frameSchedOptOut" to "frame reports to SchedGenius turned off", "colocation" to "colocation: automatic A/B",
            "threads" to "threads: watching (cluster steering on)", "gpuPath" to "hybris")
        s.display = linkedMapOf("render" to "1704x1136", "choice" to "half", "panel" to "3408x2272", "window" to "fullscreen",
            "wantedHz" to 120, "upscaler" to 0, "hdr" to false, "fpsLimit" to 0)
        s.gameProcess = "Duckov.exe"
        var frames = 0L
        for (i in 0..360) {
            val t = t0 + i * 5000L
            s.acc.add(Sample(t, i !in 100..110, true, false, frames, if (i > 250) 60f else 120f, 120f, if (i > 250) 60 else 0,
                if (i > 250) 2 else 0, 0.6f, 34f + i / 30f, 70f, 31f + i / 60f, intArrayOf(2210, 2860, 3100), intArrayOf(3600, 2400, 4200),
                mapOf("cpu" to if (i > 200) -1 else 0, "gpu" to 0, "ddr" to 0)))
            frames += 5 * 78
        }
        FrameTimes.addInto(s.hist, IntArray(FrameTimes.BINS).also { h ->
            h[FrameTimes.bin(12_500)] = 120_000; h[FrameTimes.bin(22_000)] = 1_200; h[FrameTimes.bin(3_400_000)] = 2
        })
        s.acc.memory(2_263_040, 2_000_000, 1_689_600, 419_840, 500_000, 125_829_120, 3_040_870_400, false)
        s.shellSeen = true
        s.marks += 1210L to t0 + 1_210_000
        return s
    }

    private fun finished(): MutableMap<String, Any?> {
        val s = play()
        val end = t0 + 1_830_000
        val r = PlayRecord.base(s, end, zone)
        val log = tmp.newFile("Player.log").apply {
            writeText("    Renderer: Mali-G2-Ultra-NX MC16 (ID=0x8001)\nCrash!!!\nerr:vulkan:vkCreateDevice failed, token=abcd1234efgh from 10.0.0.5\n")
        }
        val f = LogSliceScanner().scan(listOf(LogSliceScanner.Source("Player.log", log)))
        val exit = ExitClassifier.classify(PlayRecord.evidenceFrom(PlayRecord.evidence(s, end), f, usedVortek = false))
        PlayRecord.finish(r, f, exit, zone)
        return r
    }

    @Test fun aFullRecordHasEveryField() {
        val r = finished()
        assertEquals(listOf("v", "type", "id", "logs", "app", "appid", "game", "exe", "launch", "relaunchOf", "start", "end", "durS",
            "focusedS", "exit", "flags", "gpu", "env", "settings", "display", "fps", "hz", "thermal", "cpu", "mibridge", "mem", "errors", "marks",
            "timeline", "scanned"),
            r.keys.toList())
        assertEquals("2026-10-28T01:17:32+11:00", r["start"])
        assertEquals(1830L, r["durS"])
        val exit = r["exit"] as Map<*, *>
        assertEquals("crash", exit["reason"])
        assertEquals(listOf("crash", "at60_thermal", "marked"), r["flags"])
        val gpu = r["gpu"] as Map<*, *>
        assertEquals("hybris", gpu["used"])
        assertEquals("Mali-G2-Ultra-NX MC16 (ID=0x8001)", gpu["renderer"])
        val fps = r["fps"] as Map<*, *>
        assertEquals("game-layer", fps["source"])
        assertEquals(78.0, fps["mean"] as Double, 0.5)
        assertEquals(2, fps["stalls"])
        val hz = r["hz"] as Map<*, *>
        assertEquals(0.31, hz["at60"] as Double, 0.03)
        val thermal = r["thermal"] as Map<*, *>
        assertEquals("MODERATE", thermal["statusMax"])
        assertEquals("shell", thermal["source"])
        val mem = r["mem"] as Map<*, *>
        assertEquals(2210L, mem["gameHwmMB"])
        assertEquals(120L, mem["appNativeMaxMB"])
        assertEquals(listOf(mapOf("atS" to 1210L, "t" to "2026-10-28T01:37:42+11:00")), r["marks"])
    }

    @Test fun theTimelineFoldsRefreshRateFlipsAndTakesTheLogsFirstTimes() {
        val s = play()
        s.event(t0 + 30_000, "focus away (769)")
        s.event(t0 + 45_000, "focus back")
        for (i in 0 until 12) s.event(t0 + 100_000 + i * 4_000L, if (i % 2 == 0) "hz 60" else "hz 120")
        s.event(t0 + 400_000, "hz 60")
        val end = t0 + 1_830_000
        val r = PlayRecord.base(s, end, zone)
        val log = tmp.newFile("steam-1.log").apply {
            writeText("hybris-x11: window 0x4c00058 1024x768 now hidden, frames paced (t=1000.000)\n")
        }
        val uptimeToWall = t0 + 600_000 - 1_000_000
        val f = LogSliceScanner(uptimeToWallMs = uptimeToWall).scan(listOf(LogSliceScanner.Source("steam-1.log", log)))
        val exit = ExitClassifier.classify(PlayRecord.evidenceFrom(PlayRecord.evidence(s, end), f, usedVortek = false))
        PlayRecord.finish(r, f, exit, zone, startMs = t0)
        assertEquals(listOf(
            listOf(30L, "focus away (769)"), listOf(45L, "focus back"), listOf(100L, "hz 120", 12),
            listOf(400L, "hz 60"), listOf(600L, "hidden"),
        ), r["timeline"])
        assertTrue("hidden_paced" in exit.flags)
        val errors = r["errors"] as List<*>
        assertEquals(600L, (errors[0] as Map<*, *>)["atS"])
        assertEquals("hidden-paced", ((r["display"] as Map<*, *>)["present"] as String).substringAfter("+"))
    }

    @Test fun aCopyPathPlayHasNoFrameTimes() {
        val s = PlayState("x#1", "x", 1473480, "A.V.A.", "AVA/ava.exe", "steam", t0)
        var screen = 0L
        for (i in 0..120) {
            s.acc.add(Sample(t0 + i * 5000L, true, true, false, 0L, 120f, 120f, screenFrames = screen))
            screen += 600
        }
        FrameTimes.addInto(s.hist, IntArray(FrameTimes.BINS).also { it[FrameTimes.bin(3_400_000)] = 3 })
        val r = PlayRecord.base(s, t0 + 600_000, zone)
        val fps = r["fps"] as Map<*, *>
        assertEquals("screen", fps["source"])
        assertEquals(120.0, fps["mean"] as Double, 0.5)
        assertNull(fps["low1"])
        assertNull(fps["longestStallS"])
        assertEquals(0L, fps["freezeS"])
    }

    @Test fun theRecordSurvivesRedactionUnchangedAndItsErrorLinesAreScrubbed() {
        val journal = JournalFile(tmp.newFolder("journal"), LogRedactor::redact)
        val r = finished()
        val line = journal.line(r)
        assertEquals(line, LogRedactor.redact(line))
        assertFalse(line.contains("abcd1234efgh"))
        assertTrue(line.contains("<redacted:token>"))
        // The rest is exactly the record.
        val back = Json.parse(line) as Map<*, *>
        assertEquals(r.keys.toList(), back.keys.toList())
        assertEquals(Json.write((r["fps"])), Json.write(back["fps"]))
        assertEquals(Json.write((r["settings"])), Json.write(back["settings"]))
    }

    @Test fun keysStayClearOfWordsTheRedactorTreatsAsSecrets() {
        val words = listOf("session", "token", "ticket", "auth", "secret", "pass", "pwd", "user", "account", "login", "serial", "mac", "jwt")
        fun keys(v: Any?): List<String> = when (v) {
            is Map<*, *> -> v.keys.map { it.toString() } + v.values.flatMap { keys(it) }
            is List<*> -> v.flatMap { keys(it) }
            else -> emptyList()
        }
        val r = finished()
        // env and settings keys are names from elsewhere; the record's own keys are checked.
        val own = keys(r.filterKeys { it != "env" && it != "settings" })
        for (k in own) for (w in words) assertFalse("key $k holds $w", k.lowercase().contains(w))
    }

    @Test fun evidenceRoundTripsThroughOpenJson() {
        val s = play()
        val ev = PlayRecord.evidence(s, t0 + 1_830_000)
        val back = Json.parse(Json.write(ev)) as Map<*, *>
        val a = PlayRecord.evidenceFrom(ev, null, false)
        val b = PlayRecord.evidenceFrom(back, null, false)
        assertEquals(a.durS, b.durS)
        assertEquals(a.gameFrames, b.gameFrames)
        assertEquals(a.at60Thermal, b.at60Thermal)
        assertTrue(b.marked)
        assertNull(b.vortekExit)
    }
}
