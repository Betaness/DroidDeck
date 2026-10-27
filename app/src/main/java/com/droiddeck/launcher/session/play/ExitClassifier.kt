package com.droiddeck.launcher.session.play

/**
 * [mali-g2] Why a play ended, from what was seen during and after it. The first rule that matches wins;
 * the flags are independent of it.
 *
 * Steam's exit code is only a hint: the wrapper exits 0 when a Unity game crashes (its Player.log has the
 * "Crash!!!"), so a crash is decided by the logs as much as by the code.
 */
object ExitClassifier {
    /** ApplicationExitInfo of this app's process, when the play ended with the app. */
    class AppExit(val reason: String, val status: Int, val description: String?, val atMs: Long)

    class Evidence(
        val durS: Long,
        val appKilled: AppExit? = null,
        /** The app was killed and no exit record was found (or before Android 11). */
        val appGone: Boolean = false,
        val sessionStopped: Boolean = false,
        val sessionStatus: Int? = null,
        val failureCode: String? = null,
        /** vortek-server died on its own (not stopped by the session) while the play was open. */
        val vortekExit: Int? = null,
        /** Seconds from the server's death to the play's end; null when unknown. */
        val vortekToEndS: Long? = null,
        val usedVortek: Boolean = false,
        val findings: LogSliceScanner.Findings? = null,
        val steamExitCode: Int? = null,
        /** Game-layer frames during the play; null when unknown. */
        val gameFrames: Long? = null,
        val screenFramesSeen: Boolean = false,
        // For the flags.
        val longestZeroRunS: Long = 0,
        val longestStallS: Double? = null,
        val meanFpsAfterWarmup: Double? = null,
        val low1: Double? = null,
        val at60: Double? = null,
        val at60Thermal: Double? = null,
        val marked: Boolean = false,
        val runMode: Boolean = false,
    )

    class Exit(
        val reason: String,
        val steamCode: Int? = null,
        val signal: Int? = null,
        val vortek: Int? = null,
        val appExit: AppExit? = null,
        val detail: String? = null,
        val flags: List<String> = emptyList(),
    )

    fun classify(e: Evidence): Exit {
        val f = e.findings
        val crashLine = f?.firstLine("crash")
        val code = e.steamExitCode ?: e.sessionStatus.takeIf { e.runMode }
        val signal = code?.takeIf { it in 129..192 }?.let { it - 128 }
        val vortekSignal = e.vortekExit?.takeIf { it in 129..192 }?.let { it - 128 }
        val reason: String
        var detail: String? = null
        when {
            e.appKilled != null || e.appGone -> {
                reason = "app_killed"
                detail = e.appKilled?.let { a -> "app ${a.reason}" + (a.description?.takeIf { it.isNotBlank() }?.let { ": $it" } ?: "") }
                    ?: "app process gone; no exit record"
            }
            e.sessionStopped -> {
                reason = "session_stopped"
                detail = listOfNotNull(e.sessionStatus?.let { "status $it" }, e.failureCode).joinToString(", ").ifEmpty { null }
            }
            e.vortekExit != null && (e.usedVortek || (e.vortekToEndS != null && e.vortekToEndS <= 20)) -> {
                reason = "vortek_exit"
                detail = "vortek-server exited: ${e.vortekExit}" + (vortekSignal?.let { " (signal $it)" } ?: "")
            }
            f != null && f.has("device_lost") -> {
                reason = "device_lost"
                detail = f.firstLine("device_lost")
            }
            crashLine != null || (code != null && code != 0 && !(e.runMode && code < 0)) -> {
                reason = "crash"
                detail = crashLine ?: "exit code $code" + (signal?.let { " (signal $it)" } ?: "")
            }
            (e.durS < 60 && (e.gameFrames ?: 0L) == 0L && !e.screenFramesSeen) || (f != null && f.has("unsupported")) -> {
                reason = "launch_failed"
                detail = f?.firstLine("unsupported") ?: "ended within ${e.durS} s without a game frame"
            }
            else -> reason = "normal"
        }
        val flags = ArrayList<String>()
        if (reason in setOf("crash", "device_lost", "vortek_exit", "app_killed", "launch_failed")) flags += reason
        if (reason != "crash" && crashLine != null) flags += "crash"
        if (reason != "device_lost" && f?.has("device_lost") == true) flags += "device_lost"
        if (e.longestZeroRunS >= 30 || (e.longestStallS ?: 0.0) >= 10.0) flags += "freeze"
        if ((e.meanFpsAfterWarmup != null && e.meanFpsAfterWarmup < 20.0) || (e.low1 != null && e.low1 < 8.0)) flags += "low_fps"
        if (f?.has("oom") == true || e.appKilled?.reason == "LOW_MEMORY") flags += "oom"
        if (f?.has("hidden") == true) flags += "hidden_paced"
        if (f?.has("compiler") == true) flags += "compiler"
        if ((e.at60Thermal ?: 0.0) > 0.30) flags += "at60_thermal"
        if (e.marked) flags += "marked"
        return Exit(
            reason, e.steamExitCode, signal ?: vortekSignal.takeIf { reason == "vortek_exit" }, e.vortekExit,
            e.appKilled, detail, flags.distinct(),
        )
    }

    /** How long a session folder is kept for a play's flags (ms), and why; null when it needs nothing beyond the newest ten. */
    fun keepFor(flags: Collection<String>): Pair<Long, List<String>>? {
        val day = 24L * 3600 * 1000
        val why = flags.filter { it in KEEP_DAYS }
        if (why.isEmpty()) return null
        return why.maxOf { KEEP_DAYS.getValue(it) } * day to why
    }

    /** Days a session folder is kept past the newest ten, by flag. */
    val KEEP_DAYS = mapOf(
        "crash" to 14L, "device_lost" to 14L, "vortek_exit" to 14L, "freeze" to 14L, "app_killed" to 14L,
        "launch_failed" to 14L, "oom" to 14L, "compiler" to 14L, "low_fps" to 7L, "marked" to 30L,
    )
}
