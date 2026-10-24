package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.droiddeck.launcher.core.WirelessAdbFix
import kotlinx.coroutines.flow.MutableStateFlow

/**
 * [mali-g2] HyperOS's game regulator kept off the launcher on the Xring O3.
 *
 * When a process of this app first comes to the front while Xiaomi's Joyose (com.xiaomi.joyose) runs,
 * system_server's FrameSchedPolicy hands it to SchedGenius's game regulator ("setSchedGeniusIfNeed
 * start-game" in the log). The regulator sizes the clocks by this process's own render thread, idle
 * while the game runs in a child process: in one session it held the mid cores at 0.56-0.84 GHz under
 * Duckov's busy main thread (31 fps at 1704x1136), in another the GPU at 119 MHz (12 fps). It keeps
 * the process for as long as it lives: stopping Joyose afterwards, or leaving the app and coming back,
 * changed nothing. A process started while Joyose is stopped is never handed over (42-65 fps).
 *
 * So once the first activity of a process is shown, [checkAtLaunch] looks for that decision in the log
 * through the wireless debugging pairing the app keeps (Settings > Wireless debugging fix); when it is
 * there, Joyose is force-stopped and the app started again, at most once every [RESTART_GAP_MS] and
 * never while a session runs. During a session Joyose is also kept stopped ([start]), checked every
 * [CHECK_MS]. An app cannot stop another, nor can the shell user disable or suspend a system package,
 * but the shell user may force-stop it. ClusterSteer still moves the game when a clock is held.
 */
object HyperOsGameMode {
    private const val TAG = "HyperOsGameMode"
    private const val PACKAGE = "com.xiaomi.joyose"
    private const val CHECK_MS = 5_000L
    private const val DECISION_MS = 2_500L // after the activity is shown; FrameSchedPolicy decided within 0.5 s
    private const val RESTART_GAP_MS = 10 * 60_000L
    private const val COMMAND = "pidof $PACKAGE >/dev/null && am force-stop $PACKAGE && echo stopped"

    /** What the launch check found, for the setup page. */
    enum class LaunchCheck { NOT_RUN, CLEAR, RESTARTING, LEFT, NO_PAIRING, FAILED }

    val launchCheck = MutableStateFlow(LaunchCheck.NOT_RUN)

    private var worker: Thread? = null
    private var launchChecked = false

    /** Whether the regulator is looked for here: the Xring O3, with the setting on. */
    fun applies(context: Context) = onO3() && SessionPrefs.stopJoyose(context)

    /** At a session's start. The line is for the log. */
    @Synchronized
    fun start(context: Context): String {
        if (!SessionPrefs.stopJoyose(context)) return "joyose: left running (setting off)"
        if (!onO3()) return "joyose: not on this SoC"
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

    /** When the first activity of this process is shown. */
    @Synchronized
    fun checkAtLaunch(context: Context) {
        if (launchChecked) return
        launchChecked = true
        if (!applies(context)) return
        val app = context.applicationContext
        val shownAt = SystemClock.elapsedRealtime()
        Thread({ checkRegulator(app, shownAt) }, "hyperos-game-regulator").apply { isDaemon = true; start() }
    }

    /** Right after a pairing: this process was shown long ago, the decision is in the log already. */
    fun checkNow(context: Context) {
        if (!applies(context)) return
        val app = context.applicationContext
        Thread({ checkRegulator(app, 0L) }, "hyperos-game-regulator").apply { isDaemon = true; start() }
    }

    private fun onO3() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    private fun checkRegulator(context: Context, shownAt: Long) {
        try {
            val adb = WirelessAdbFix.connectUsingSavedPairing(context)
            if (adb == null) {
                Log.i(TAG, "no wireless debugging pairing, or wireless debugging is off: the game regulator is not checked")
                launchCheck.value = LaunchCheck.NO_PAIRING
                return
            }
            adb.use {
                Thread.sleep((shownAt + DECISION_MS - SystemClock.elapsedRealtime()).coerceAtLeast(0))
                val pkg = context.packageName
                val decision = it.shell(
                    "logcat -d -s FrameSchedPolicy:D | grep -F 'setSchedGeniusIfNeed start-' | " +
                        "grep -F 'appName:$pkg ' | grep -F -- '-${Process.myPid()}]' | tail -n 1"
                ).output.trim()
                if (decision.isEmpty()) {
                    Log.i(TAG, "not handed to the game regulator")
                    launchCheck.value = LaunchCheck.CLEAR
                    return
                }
                val prefs = context.getSharedPreferences("session", Context.MODE_PRIVATE)
                val last = prefs.getLong("regulatorRestartAt", 0L)
                val now = System.currentTimeMillis()
                launchCheck.value = LaunchCheck.LEFT
                when {
                    SessionState.phase != SessionPhase.IDLE ->
                        Log.i(TAG, "handed to the game regulator, but a session runs: left as it is")
                    now - last in 0 until RESTART_GAP_MS ->
                        Log.i(TAG, "handed to the game regulator again after a restart: left as it is")
                    else -> {
                        val launcher = context.packageManager.getLaunchIntentForPackage(pkg)?.component
                        if (launcher == null) {
                            Log.w(TAG, "handed to the game regulator; no launcher activity to start again")
                            return
                        }
                        prefs.edit().putLong("regulatorRestartAt", now).commit()
                        launchCheck.value = LaunchCheck.RESTARTING
                        Log.i(TAG, "handed to the game regulator (${decision.substringAfter("setSchedGeniusIfNeed ").substringBefore(' ')}): " +
                            "stopping Joyose and starting again")
                        // Detached, so it outlives this process and the connection.
                        it.shell(
                            "setsid sh -c 'am force-stop $PACKAGE; am force-stop $pkg; " +
                                "am start -n ${launcher.flattenToShortString()}' </dev/null >/dev/null 2>&1 &"
                        )
                    }
                }
            }
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "game regulator check failed", t)
            launchCheck.value = LaunchCheck.FAILED
        }
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
