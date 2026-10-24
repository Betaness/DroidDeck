package com.droiddeck.launcher.session

import android.content.Context
import com.droiddeck.launcher.runtime.LinuxRuntime
import java.io.File

/**
 * [mali-g2] In-process Vulkan for games (mali-g2/hybris): a game's processes load the vendor Mali
 * driver themselves through libhybris and show their frames on the compositor's zero-copy layers,
 * instead of forwarding every call to [VortekServerComponent]; the Steam client stays on Vortek.
 *
 * Off by default (session pref gpuPath = "hybris" turns it on). The session then sees the Android
 * system tree (the driver, Android's libvulkan and libnativewindow come from it) and gets [ENV];
 * droiddeck-game-env points the game's Vulkan loader at [GUEST_ICD] and preloads libhybris-common.
 * The guest files come from the APK ([SessionFiles]), built by mali-g2/hybris/build.sh.
 */
object HybrisGpu {
    const val PATH = "hybris"
    const val ENV = "DROIDDECK_GPU_PATH"
    const val GUEST_DIR = "/usr/local/lib/droiddeck-hybris"
    const val GUEST_ICD = "$GUEST_DIR/droiddeck_hybris_icd.aarch64.json"

    fun wanted(context: Context): Boolean = SessionPrefs.gpuPath(context) == PATH

    fun installed(context: Context): Boolean = File(LinuxRuntime.rootDir(context), GUEST_ICD.removePrefix("/")).isFile
}
