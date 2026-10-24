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
 *
 * What SchedGenius then measures is the time between two frames shown, not the game's render time
 * (libmigui dequeues when the app starts drawing; the compositor only sees frames arrive): a game
 * held at its limit looks like it uses the whole frame budget, and jitter reads as missed frames,
 * which may keep the clocks up for nothing. See frame_sched_feed.c.
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
        if (!SessionPrefs.feedFrameSched(context)) {
            off()
            return "frame governor feed: off"
        }
        if (Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "O3") {
            off()
            return "frame governor feed: not on this SoC"
        }
        val asked = SessionPrefs.feedFrameSchedThread(context) == "game"
        // Only GameThreadHints' scan names the game's thread.
        val want = if (asked && SessionPrefs.gameThreadHints(context)) GAME_THREAD else COMPOSITOR
        return try {
            WaylandCompositor.nativeSetFrameSchedThread(0, 0)
            WaylandCompositor.nativeSetFrameSchedFeed(want)
            mode = want
            "frame governor feed: game layer frames to SchedGenius as " +
                when {
                    want == GAME_THREAD -> "the game's busiest thread once GameThreadHints names one (the compositor thread until then)"
                    asked -> "the compositor thread (the game's thread needs gameThreadHints on)"
                    else -> "the compositor thread"
                } +
                (if (SessionPrefs.frameSchedOptOut(context)) "" else " (libmigui's own reports left on too)")
        } catch (t: Throwable) {
            Log.w(TAG, "start", t)
            "frame governor feed: unavailable"
        }
    }

    /** The native side off whatever the last session left, without relying on its stop(). */
    private fun off() {
        mode = 0
        try {
            WaylandCompositor.nativeSetFrameSchedFeed(0)
            WaylandCompositor.nativeSetFrameSchedThread(0, 0)
        } catch (t: Throwable) {
            Log.w(TAG, "off", t)
        }
    }

    /** At a session's end. */
    @Synchronized
    fun stop() {
        if (mode == 0) return
        off()
    }

    /** From GameThreadHints' scan, once a second: the game's busiest thread [tid] of process [pid]. */
    fun gameThread(pid: Int, tid: Int) {
        if (mode == GAME_THREAD) runCatching { WaylandCompositor.nativeSetFrameSchedThread(pid, tid) }
    }
}
