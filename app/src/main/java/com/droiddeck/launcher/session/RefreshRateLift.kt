package com.droiddeck.launcher.session

import android.content.Context
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Build
import android.provider.Settings
import android.util.Log
import android.view.Display
import com.droiddeck.launcher.core.WirelessAdbFix
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * [mali-g2] The panel's top refresh rate (144 Hz) while a game session is on screen on the Xring O3 (on by default there).
 *
 * The session asks for the panel's top mode (PerfMode), but HyperOS gives a third-party app no more than the rate chosen
 * in Settings > Display, Secure user_refresh_rate (120 Hz): PowerKeeper writes it for the foreground app to Secure
 * miui_refresh_rate, and DisplayModeDirector caps rendering there. The app may not write Secure settings, the shell user
 * may: so it is raised through the wireless debugging pairing while a session activity is shown, and put back afterwards
 * ([RefreshRateLiftLogic] has the rules). On the O3 the panel went to 144 Hz within about a second; Duckov's frame rate,
 * pacing and temperatures stayed as they were.
 *
 * Shell calls run on one background thread, in order. Without a working pairing nothing changes, and that is logged
 * once per session.
 */
object RefreshRateLift {
    private const val TAG = "RefreshRateLift"
    /** Session prefs: the value written; set while this app has the setting raised. */
    internal const val WRITTEN = "refreshLiftWritten"
    /** Session prefs: the value before, absent when it was unset. */
    internal const val ORIGINAL = "refreshLiftOriginal"
    /** Session activities stopping and starting again (recreated, a dialog over them) keep the setting raised. */
    private const val HIDE_GRACE_MS = 1_500L
    /** Watched for re-pacing when the app may read them; the display's mode changes are followed in any case. */
    val WATCHED = listOf(RefreshRateLiftLogic.KEY, "miui_refresh_rate")

    private val executor = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "refresh-lift").apply { isDaemon = true } }
    private var pendingHide: ScheduledFuture<*>? = null
    // Used on the executor's thread only.
    private var shell: Shell? = null
    private var logic: RefreshRateLiftLogic? = null
    private var failureLogged = false

    val available: Boolean get() = Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL == "O3"

    /** For the settings page: whether the setting is on, or null where it is not offered (the panel is no faster than the rate chosen in Settings). */
    fun setting(context: Context): Boolean? {
        if (!available) return null
        val record = Prefs(context).load()
        // Raised here, the value before counts; unreadable to the app, it is offered (the pairing reads it).
        val chosen = if (record != null) record.original
            else runCatching { Settings.Secure.getString(context.contentResolver, RefreshRateLiftLogic.KEY) }.getOrNull()
        return if (RefreshRateLiftLogic.below(chosen, topHz(context))) SessionPrefs.refreshLift(context) else null
    }

    fun setEnabled(context: Context, on: Boolean) {
        SessionPrefs.setRefreshLift(context, on)
        if (!on && available) submit(context) { _, logic -> logic.turnedOff() }
    }

    /** A session activity started. */
    @Synchronized
    fun shown(context: Context) {
        if (!available) return
        pendingHide?.cancel(false)
        pendingHide = null
        submit(context) { app, logic -> logic.shown(SessionPrefs.refreshLift(app), topHz(app)) }
    }

    /** No session activity is started any more. */
    @Synchronized
    fun hidden(context: Context) {
        if (!available) return
        val app = context.applicationContext
        pendingHide?.cancel(false)
        pendingHide = executor.schedule({ run(app) { _, logic -> logic.hidden() } }, HIDE_GRACE_MS, TimeUnit.MILLISECONDS)
    }

    /** At a session's start. The line is for the log. */
    fun sessionStarted(context: Context): String {
        if (!available) return "refresh lift: not on this SoC"
        val on = SessionPrefs.refreshLift(context)
        submit(context) { app, logic -> logic.sessionStarted(on, topHz(app)) }
        return if (on) "refresh lift: ${RefreshRateLiftLogic.KEY} raised to the panel's top rate while on screen (see the $TAG lines)"
        else "refresh lift: off"
    }

    /** At a session's end. */
    fun sessionEnded(context: Context) {
        if (!available) return
        submit(context) { _, logic -> logic.sessionEnded() }
        executor.execute { failureLogged = false }
    }

    /** At app start: a value left raised by a process that died is put back. */
    fun appStarted(context: Context) {
        if (!available || Prefs(context).load() == null) return
        submit(context) { _, logic -> logic.appStarted() }
    }

    /** Has [observer] told of changes to the [WATCHED] settings this app may read. */
    fun watch(context: Context, observer: ContentObserver) {
        for (name in WATCHED) {
            if (runCatching { Settings.Secure.getString(context.contentResolver, name) }.isFailure) continue
            context.contentResolver.registerContentObserver(Settings.Secure.getUriFor(name), false, observer)
        }
    }

    /** The default display's top refresh rate at its current size, 0 when unknown. */
    internal fun topHz(context: Context): Int {
        val display = context.getSystemService(DisplayManager::class.java)?.getDisplay(Display.DEFAULT_DISPLAY) ?: return 0
        val mode = display.mode
        val top = display.supportedModes
            .filter { it.physicalWidth == mode.physicalWidth && it.physicalHeight == mode.physicalHeight }
            .maxOfOrNull { it.refreshRate } ?: return 0
        return Math.round(top)
    }

    private fun submit(context: Context, block: (Context, RefreshRateLiftLogic) -> String?) {
        val app = context.applicationContext
        executor.execute { run(app, block) }
    }

    private fun run(app: Context, block: (Context, RefreshRateLiftLogic) -> String?) {
        val shell = shell ?: Shell(app).also { shell = it }
        val logic = logic ?: RefreshRateLiftLogic(shell, Prefs(app)).also { logic = it }
        try {
            block(app, logic)?.let { Log.i(TAG, it) }
        } catch (t: Throwable) {
            if (!failureLogged) {
                failureLogged = true
                if (t is NoPairing) Log.i(TAG, "${t.message}: ${RefreshRateLiftLogic.KEY} left as it is")
                else Log.w(TAG, "${RefreshRateLiftLogic.KEY} left as it is", t)
            }
        } finally {
            shell.close()
        }
    }

    private class NoPairing : Exception("no wireless debugging pairing, or wireless debugging is off")

    /** The setting through the pairing; connected on first use, closed after each call of [run]. */
    private class Shell(private val context: Context) : RefreshRateLiftLogic.Secure {
        private var adb: com.flyfishxu.kadb.Kadb? = null

        private fun command(line: String): String {
            val adb = adb ?: (WirelessAdbFix.connectUsingSavedPairing(context) ?: throw NoPairing()).also { adb = it }
            val result = adb.shell(line)
            check(result.exitCode == 0) { "'$line' failed (${result.exitCode}): ${result.allOutput.trim().take(200)}" }
            return result.output
        }

        override fun read() = RefreshRateLiftLogic.parse(command("settings get secure ${RefreshRateLiftLogic.KEY}"))

        override fun put(value: String) {
            require(RefreshRateLiftLogic.isRate(value)) { "not a rate: '$value'" }
            command("settings put secure ${RefreshRateLiftLogic.KEY} $value")
        }

        override fun delete() {
            command("settings delete secure ${RefreshRateLiftLogic.KEY}")
        }

        fun close() {
            adb?.let { runCatching { it.close() } }
            adb = null
        }
    }

    private class Prefs(context: Context) : RefreshRateLiftLogic.Records {
        private val prefs = context.applicationContext.getSharedPreferences("session", Context.MODE_PRIVATE)

        override fun load() = prefs.getString(WRITTEN, null)?.let { RefreshRateLiftLogic.Record(prefs.getString(ORIGINAL, null), it) }

        override fun save(record: RefreshRateLiftLogic.Record) {
            val edit = prefs.edit().putString(WRITTEN, record.written)
            if (record.original == null) edit.remove(ORIGINAL) else edit.putString(ORIGINAL, record.original)
            check(edit.commit()) { "could not record ${RefreshRateLiftLogic.KEY}" }
        }

        override fun clear() {
            prefs.edit().remove(WRITTEN).remove(ORIGINAL).commit()
        }
    }
}
