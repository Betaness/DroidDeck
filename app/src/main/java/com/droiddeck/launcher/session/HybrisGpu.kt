package com.droiddeck.launcher.session

import android.content.Context
import com.droiddeck.launcher.runtime.LinuxRuntime
import java.io.File

/**
 * [mali-g2] In-process Vulkan for games (mali-g2/hybris): a game's processes load the vendor Mali
 * driver themselves through libhybris and show their frames on the compositor's zero-copy layers,
 * instead of forwarding every call to [VortekServerComponent]; the Steam client stays on Vortek.
 *
 * Session pref gpuPath = "hybris" turns it on, "vortek" off; unset, it is on where it was measured, the Xring O3
 * (half-resolution Duckov title 88.2 instead of 78.5 fps, no crash in 13 sessions). The session then sees the Android
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

    /** The pref's value, or the default when it is unset or blank: [PATH] on the O3, "vortek" elsewhere. */
    internal fun path(stored: String?, onO3: Boolean): String =
        stored?.trim()?.takeIf { it.isNotEmpty() } ?: if (onO3) PATH else "vortek"

    fun installed(context: Context): Boolean = File(LinuxRuntime.rootDir(context), GUEST_ICD.removePrefix("/")).isFile
}
