package com.droiddeck.launcher.session

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GameSavesTest {
    @Test fun `caches stay out of save zips`() {
        assertTrue(GameSaves.notSaves("AppData/Local/fex-emu/DiskCache"))
        assertTrue(GameSaves.notSaves("AppData/Roaming/FEX-Emu/diskcache"))
        assertTrue(GameSaves.notSaves("AppData/Local/Temp"))
        assertTrue(GameSaves.notSaves("appdata/local/d3dscache"))
        assertTrue(GameSaves.notSaves("AppData/Local/UnrealEngine/Common/DerivedDataCache"))
        assertTrue(GameSaves.notSaves("AppData/Local/Microsoft/Windows/INetCache"))
    }

    @Test fun `save folders are kept`() {
        assertFalse(GameSaves.notSaves("AppData/Local/fex-emu"))
        assertFalse(GameSaves.notSaves("AppData/Local/fex-emu/AppConfig"))
        assertFalse(GameSaves.notSaves("AppData/Local/SomeGame/DiskCache"))  // a game's own folder of that name
        assertFalse(GameSaves.notSaves("AppData/Local/Microsoft"))
        assertFalse(GameSaves.notSaves("Documents/My Games/Game/Saves"))
        assertFalse(GameSaves.notSaves(""))
    }
}
