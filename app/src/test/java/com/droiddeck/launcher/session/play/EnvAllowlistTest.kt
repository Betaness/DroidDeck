package com.droiddeck.launcher.session.play

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class EnvAllowlistTest {
    @Test fun onlyTheSwitchesThatChangeHowAGameRunsAreKept() {
        val env = EnvAllowlist.filter(listOf(
            "SteamUser=someone", "USER=root", "HOME=/root", "SteamAppId=3167020", "STEAM_COMPAT_CLIENT_INSTALL_PATH=/root/x",
            "DXVK_HUD=fps", "PROTON_NO_NTSYNC=1", "WINEESYNC=1", "BL_SYNC=pool", "DROIDDECK_GPU_PATH=hybris",
            "VORTEK_CMD_RELEASE_EVERY=32", "PATH=/usr/bin", "DISPLAY=:0", "XAUTHORITY=/tmp/x", "=junk", "noequals",
        ))
        assertEquals(
            listOf("BL_SYNC", "DROIDDECK_GPU_PATH", "DXVK_HUD", "PROTON_NO_NTSYNC", "SteamAppId", "VORTEK_CMD_RELEASE_EVERY", "WINEESYNC"),
            env.keys.toList(),
        )
        assertFalse(EnvAllowlist.allowed("SteamUser"))
        assertTrue(EnvAllowlist.allowed("DXVK_ASYNC"))
    }

    @Test fun libraryPathsBecomeFileNames() {
        val env = EnvAllowlist.filter(listOf(
            "LD_PRELOAD=/usr/local/lib/libhybris-preload.so:/opt/fake/libfakeinput.so",
            "VK_ICD_FILENAMES=/usr/local/lib/droiddeck-vortek/vortek_icd.aarch64.json",
        ))
        assertEquals("libhybris-preload.so,libfakeinput.so", env["LD_PRELOAD"])
        assertEquals("vortek_icd.aarch64.json", env["VK_ICD_FILENAMES"])
    }

    @Test fun valuesAndKeysAreCapped() {
        val env = EnvAllowlist.filter((1..100).map { "DXVK_K%03d=%s".format(it, "v".repeat(300)) })
        assertEquals(EnvAllowlist.MAX_KEYS, env.size)
        assertEquals(EnvAllowlist.MAX_VALUE, env.values.first().length)
    }
}
