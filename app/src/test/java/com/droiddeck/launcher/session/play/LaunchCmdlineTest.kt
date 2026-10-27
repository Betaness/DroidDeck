package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class LaunchCmdlineTest {
    @get:Rule val tmp = TemporaryFolder()

    // As gameprocess_log.txt shows the wrapper (2026-10-28-07-steam), split into argv.
    private val wrapper = listOf(
        "/root/.local/share/Steam/linuxarm64/steam-launch-wrapper", "--oom-score-adjust", "900", "--",
        "/root/.local/share/Steam/steamrtarm64/reaper", "SteamLaunch", "AppId=3167020", "--",
        "/root/.local/share/Steam/compatibilitytools.d/droiddeck-proton-arm64/droiddeck-proton", "waitforexitandrun",
        "/root/.local/share/Steam/steamapps/common/Escape from Duckov/Duckov.exe",
    )

    @Test fun protonLaunchGivesTheAppIdAndTheExeBelowCommon() {
        val l = LaunchCmdline.parse(wrapper)!!
        assertEquals(3167020L, l.appid)
        assertEquals("Escape from Duckov/Duckov.exe", l.exe)
        assertEquals("Duckov.exe", l.exeName)
        // The reaper carries the same.
        assertEquals(3167020L, LaunchCmdline.parse(wrapper.drop(4))!!.appid)
    }

    @Test fun argumentsAfterTheExeAreIgnored() {
        val l = LaunchCmdline.parse(wrapper.dropLast(1) + listOf(
            "/root/.local/share/Steam/steamapps/common/A.V.A/Binaries/start_protected_game.exe", "-serveraddr\"x:1\""))!!
        assertEquals("A.V.A/Binaries/start_protected_game.exe", l.exe)
    }

    @Test fun installScriptsAndOtherVerbsAreNotPlays() {
        val run = wrapper.toMutableList().apply { this[9] = "run" }
        assertNull(LaunchCmdline.parse(run))
        assertNull(LaunchCmdline.parse(listOf("/usr/bin/python3", "proton", "waitforexitandrun", "x.exe")))
        assertNull(LaunchCmdline.parse(listOf("reaper", "SteamLaunch", "AppId=0", "--", "x")))
    }

    @Test fun nativeGamesAndShortcuts() {
        val native = LaunchCmdline.parse(listOf("reaper", "SteamLaunch", "AppId=70", "--",
            "/root/.local/share/Steam/steamapps/common/Half-Life/hl_linux", "-steam"))!!
        assertEquals("Half-Life/hl_linux", native.exe)
        val shortcut = LaunchCmdline.parse(listOf("reaper", "SteamLaunch", "AppId=3221225472", "--",
            "/x/droiddeck-proton", "waitforexitandrun", "/sdcard/Games/Some Game/game.exe"))!!
        assertEquals(3221225472L, shortcut.appid)
        assertEquals("Some Game/game.exe", shortcut.exe)
    }

    @Test fun wineShowsTheWindowsPathAsArgv0() {
        assertTrue(LaunchCmdline.runs("C:\\Program Files\\Duckov\\Duckov.exe", "Duckov.exe"))
        assertTrue(LaunchCmdline.runs("Z:\\root\\x\\DUCKOV.EXE", "Duckov.exe"))
        assertFalse(LaunchCmdline.runs("C:\\windows\\system32\\services.exe", "Duckov.exe"))
    }

    private fun proc(root: File, pid: Int, argv: List<String>, start: Long, status: String = "", environ: List<String> = emptyList()) {
        val d = File(root, "$pid").apply { mkdirs() }
        File(d, "cmdline").writeBytes((argv.joinToString("\u0000") + "\u0000").toByteArray())
        val fields = (4..21).map { "0" }
        File(d, "stat").writeText("$pid (some (name)) S " + fields.joinToString(" ") + " $start 0 0\n")
        File(d, "status").writeText(status)
        File(d, "environ").writeBytes((environ.joinToString("\u0000") + "\u0000").toByteArray())
    }

    @Test fun scannerGroupsALaunchAndFindsTheGameProcess() {
        val root = tmp.newFolder("proc")
        proc(root, 100, listOf("/system/bin/app_process64"), 10)
        proc(root, 24020, wrapper, 500)
        proc(root, 24022, wrapper.drop(4), 501)
        proc(root, 24248, listOf("C:\\Duckov\\Duckov.exe"), 520, "Name:\tDuckov.exe\nVmHWM:\t 2210000 kB\nVmRSS:\t1900000 kB\nRssAnon:\t1600000 kB\n",
            listOf("DXVK_ASYNC=1", "SteamUser=someone", "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/hybris_icd.json"))
        File(root, "self").mkdirs()
        val scanner = ProcScanner(root)
        val entries = scanner.scan()
        assertEquals(setOf(100, 24020, 24022, 24248), entries.keys)
        assertEquals(500L, entries.getValue(24020).startTicks)
        val launches = ProcScanner.launches(entries)
        assertEquals(setOf(3167020L), launches.keys)
        val g = launches.getValue(3167020L)
        assertEquals(setOf(24020, 24022), g.pids)
        assertEquals(500L, g.startTicks)
        assertEquals(24248, ProcScanner.gameProcess(entries, "Duckov.exe")!!.pid)
        assertEquals(2210000L, scanner.statusKb(24248, "VmHWM", "RssAnon")["VmHWM"])
        assertEquals(1600000L, scanner.statusKb(24248, "VmHWM", "RssAnon")["RssAnon"])
        val env = EnvAllowlist.filter(scanner.environ(24248)!!)
        assertEquals(mapOf("DXVK_ASYNC" to "1", "VK_ICD_FILENAMES" to "hybris_icd.json"), env)

        File(root, "24020").deleteRecursively()
        File(root, "24022").deleteRecursively()
        assertTrue(ProcScanner.launches(scanner.scan()).isEmpty())
    }
}
