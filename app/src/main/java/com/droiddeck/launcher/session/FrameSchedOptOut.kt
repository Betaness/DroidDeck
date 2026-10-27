package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.util.Log

/**
 * [mali-g2] Keeps HyperOS's per-layer frame governor away from the GPU clock on the Xring O3.
 *
 * Once Joyose has reported this package as a game, system_server names it the top game in
 * /data/system/mcd/fs and only clears that when Joyose sees the process die, which it rarely does here
 * (HyperOsGameMode stops Joyose). Every process of the package then reports its BufferQueue frames to
 * SchedGenius through libmigui, and SchedGenius sets the GPU clock pair from those layers: the session
 * window and the idle SurfaceView, while the game's frames go to child SurfaceControls and its GPU work
 * comes from the Vortek server. Duckov at 1704x1136 sat at 11-17 fps with the GPU held at 119-179 MHz for
 * the first 30-60 s of most sessions (until SchedGenius dropped the idle layer) and for whole copy-path
 * sessions; with the reports off it ran at 60-68 fps with the GPU at about 1.1 GHz from the title on.
 */
object FrameSchedOptOut {
    private const val TAG = "FrameSchedOptOut"

    @JvmStatic private external fun nativeDisable(process: String): Int

    /** [mali-g2] What [apply] did in this process, for the play journal; null when it did not run (not the O3). */
    @Volatile
    var outcome: String? = null
        private set

    /** At process start, before any window draws. */
    fun apply(context: Context, process: String) {
        if (Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "O3") return
        if (!SessionPrefs.frameSchedOptOut(context)) {
            outcome = "frame reports to SchedGenius left on (setting off)"
            Log.i(TAG, "frame reports to SchedGenius left on (setting off)")
            return
        }
        val result = try {
            System.loadLibrary("sdnative")
            nativeDisable(process)
        } catch (t: Throwable) {
            outcome = "frame reports to SchedGenius left on (${t.javaClass.simpleName})"
            Log.w(TAG, "frame reports to SchedGenius left on", t)
            return
        }
        val line = when (result) {
            1 -> "frame reports to SchedGenius turned off"
            0 -> "libmigui not loaded: nothing to turn off"
            else -> "libmigui's FrameSchedPolicy does not look as expected ($result): left as it is"
        }
        outcome = line
        Log.i(TAG, line)
    }
}
