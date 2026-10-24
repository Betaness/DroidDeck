package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.util.Log
import com.droiddeck.launcher.wayland.WaylandCompositor

/**
 * [mali-g2] Reports the game's frames to HyperOS's frame governor (SchedGenius) on the Xring O3.
 *
 * FrameSchedOptOut keeps libmigui from reporting this app's BufferQueue frames (the session window and
 * the idle SurfaceView), which held the GPU at 119-179 MHz; with nothing reported the clocks sit where
 * the default governors put them (mid cores ~2.3 GHz, the GPU ~1 GHz at 90% busy). SchedGenius raises
 * a layer's clocks when its frames miss their expected time, so the compositor reports the game's own
 * layer (droiddeck_vortek_game, zero-copy only) through /dev/xr_fbt as libmigui would: the session's
 * frame-rate limit or the panel's rate as the target, a queue with the frame's acquire fence and the
 * next frame's dequeue per frame shown. The thread named is the compositor's, or with "game" the game's
 * busiest thread from GameThreadHints, where the kernel takes another process's thread.
 */
object FrameSchedFeed {
    private const val TAG = "FrameSchedFeed"
    private const val COMPOSITOR = 1
    private const val GAME_THREAD = 2

    @Volatile
    private var mode = 0

    /** At a session's start. The line is for the log. */
    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.feedFrameSched(context)) return "frame governor feed: off"
        if (Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "O3") return "frame governor feed: not on this SoC"
        val want = if (SessionPrefs.feedFrameSchedThread(context) == "game") GAME_THREAD else COMPOSITOR
        return try {
            WaylandCompositor.nativeSetFrameSchedThread(0, 0)
            WaylandCompositor.nativeSetFrameSchedFeed(want)
            mode = want
            "frame governor feed: game layer frames to SchedGenius as " +
                (if (want == GAME_THREAD) "the game's busiest thread" else "the compositor thread") +
                (if (SessionPrefs.frameSchedOptOut(context)) "" else " (libmigui's own reports left on too)")
        } catch (t: Throwable) {
            Log.w(TAG, "start", t)
            "frame governor feed: unavailable"
        }
    }

    /** At a session's end. */
    @Synchronized
    fun stop() {
        if (mode == 0) return
        mode = 0
        try {
            WaylandCompositor.nativeSetFrameSchedFeed(0)
            WaylandCompositor.nativeSetFrameSchedThread(0, 0)
        } catch (t: Throwable) {
            Log.w(TAG, "stop", t)
        }
    }

    /** From GameThreadHints' scan, once a second: the game's busiest thread [tid] of process [pid]. */
    fun gameThread(pid: Int, tid: Int) {
        if (mode == GAME_THREAD) WaylandCompositor.nativeSetFrameSchedThread(pid, tid)
    }
}
