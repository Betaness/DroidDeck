package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.util.Log
import com.droiddeck.launcher.core.WirelessAdbFix

/**
 * [mali-g2] Xiaomi's Joyose kept stopped while a session runs on the Xring O3.
 *
 * Joyose (com.xiaomi.joyose) hosts SmartGamingClient: when libmigui's GameRenderDetector reports the
 * launcher's render thread, it starts SchedGenius's game regulator, which sized the clocks by that
 * idle thread and held the mid cores at 0.56-0.84 GHz under Duckov's busy main thread (27-33 fps at
 * 1704x1136; 59-61 fps with Joyose stopped). An app cannot stop another, nor can the shell user
 * disable or suspend a system package, but the shell user may force-stop it: through the wireless
 * debugging pairing the app already keeps (Settings > Wireless debugging fix), Joyose is stopped at the
 * session's start and again whenever it came back, checked every [CHECK_MS]. It is not disabled:
 * Android starts it again when something binds it, and it is back after a reboot in any case.
 * ClusterSteer still moves the game when the clock is held some other way.
 */
object HyperOsGameMode {
    private const val TAG = "HyperOsGameMode"
    private const val PACKAGE = "com.xiaomi.joyose"
    private const val CHECK_MS = 5_000L
    private const val COMMAND = "pidof $PACKAGE >/dev/null && am force-stop $PACKAGE && echo stopped"

    private var worker: Thread? = null

    /** At a session's start. The line is for the log. */
    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.stopJoyose(context)) return "joyose: left running (setting off)"
        if (Build.SOC_MODEL != "O3") return "joyose: not on this SoC"
        val app = context.applicationContext
        worker?.interrupt()
        worker = Thread({ run(app) }, "hyperos-game-mode").apply { isDaemon = true; start() }
        return "joyose: stopping it during the session"
    }

    /** At a session's end. */
    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker = null
    }

    private fun run(context: Context) {
        val adb = WirelessAdbFix.connectUsingSavedPairing(context)
        if (adb == null) {
            Log.i(TAG, "no wireless debugging pairing, or wireless debugging is off: Joyose keeps running")
            return
        }
        var stops = 0
        try {
            adb.use {
                while (!Thread.currentThread().isInterrupted) {
                    if (it.shell(COMMAND).output.contains("stopped") && stops++ < 20) Log.i(TAG, "stopped Joyose")
                    Thread.sleep(CHECK_MS)
                }
            }
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped watching", t)
        }
    }
}
