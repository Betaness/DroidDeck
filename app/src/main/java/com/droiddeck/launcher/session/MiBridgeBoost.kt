package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.util.Log
import com.droiddeck.launcher.core.WirelessAdbFix
import dalvik.system.PathClassLoader
import java.lang.reflect.Method

/**
 * [mali-g2] Clock floors from Xiaomi's MiBridge during a session on the Xring O3 (experimental, off by default).
 *
 * HyperOS serves third-party apps a performance SDK, com.miui.performance.MiuiBooster in
 * /system_ext/framework/MiuiBooster.jar. Its requests reach system_server's MirimThirdPartyPolicy, which
 * passes CPU, GPU and DDR "frequency levels" (1-3) to the uniperf HAL for at most 10, 5 or 1.5 seconds per
 * request (level 1, 2, 3); a CPU request is refused while the board sensor reads 40 C or more. Only packages
 * in the system setting mibridge_authorized_pkg_list (a cloud-fed list) are served. The app may not edit
 * that setting, the shell user may: so the package is added through the wireless debugging pairing at a
 * session's start, and taken out again at the first session after the setting here is cleared (and the
 * pairing works; clearing the app's data or uninstalling it leaves the package listed). Being listed lets the
 * app ask MirimThirdPartyPolicy; PowerKeeper (MiSpeedUtils) also hands the listed uids to the native
 * miuiboosterservice, and skips that while the list is empty or unset, so when this package was the only
 * entry its uid stays there until a reboot after it is taken out. The floors are system-wide clock requests,
 * so the game in the session's child processes gets them too; they lapse by themselves when the app stops
 * renewing them. A cloud update rewriting the list takes the grant away mid-session: a request refused for
 * want of it has the package listed and the permission checked again, at most once a minute.
 *
 * The setting [SessionPrefs.miBridgeBoost] names what to hold, e.g. "gpu=1" or "gpu=1,ddr=1,cpu=1";
 * empty is off. Which clocks the levels map to is not known yet: uniperf's tables are not readable.
 */
object MiBridgeBoost {
    private const val TAG = "MiBridgeBoost"
    private const val SETTING = "mibridge_authorized_pkg_list"
    private const val JAR = "/system_ext/framework/MiuiBooster.jar"
    private const val ADDED = "miBridgeAuthorized" // session prefs: this app put itself in SETTING
    private const val POLL_MS = 250L
    private const val REFUSED_RETRY_MS = 5_000L
    private const val REGRANT_EVERY_MS = 60_000L
    /** MiuiBooster's own refusal: its static permission flag is unset, or the uid is not the caller's. */
    private const val NOT_PERMITTED = -2
    private val MAX_TIMEOUT_MS = intArrayOf(0, 10_000, 5_000, 1_500)
    private val PACKAGE_LIST = Regex("[A-Za-z0-9._,]*")

    internal data class Hold(val kind: String, val level: Int) {
        val method get() = kind.replaceFirstChar { it.uppercaseChar() }
        val timeoutMs get() = MAX_TIMEOUT_MS[level]
        /** Renewed halfway: level 3's 1.5 s leaves a few binder and HAL calls no room nearer its end. */
        val renewMs get() = timeoutMs / 2L
    }

    @Volatile
    private var worker: Thread? = null

    /** "gpu=1,cpu=2" as holds; null when malformed. Each kind once, levels 1-3. */
    internal fun parse(spec: String): List<Hold>? {
        val holds = spec.split(',').map { it.trim() }.filter { it.isNotEmpty() }.map { part ->
            val (kind, level) = part.split('=').takeIf { it.size == 2 } ?: return null
            val l = level.trim().toIntOrNull()?.takeIf { it in 1..3 } ?: return null
            Hold(kind.trim().lowercase().takeIf { it in setOf("cpu", "gpu", "ddr") } ?: return null, l)
        }
        return holds.takeIf { it.isNotEmpty() && it.distinctBy { h -> h.kind }.size == it.size }
    }

    /** [list] with [pkg] added, or null when it is there already. */
    internal fun withPackage(list: String, pkg: String): String? {
        val pkgs = list.split(',').map { it.trim() }.filter { it.isNotEmpty() }
        return if (pkg in pkgs) null else (pkgs + pkg).joinToString(",")
    }

    /** [list] without [pkg], or null when it is not there. */
    internal fun withoutPackage(list: String, pkg: String): String? {
        val pkgs = list.split(',').map { it.trim() }.filter { it.isNotEmpty() }
        return if (pkg !in pkgs) null else pkgs.filter { it != pkg }.joinToString(",")
    }

    /** At a session's start. The line is for the log. */
    @Synchronized
    fun start(context: Context): String {
        val app = context.applicationContext
        val spec = SessionPrefs.miBridgeBoost(app)
        if (spec.isBlank()) {
            if (app.getSharedPreferences("session", Context.MODE_PRIVATE).getBoolean(ADDED, false)) {
                Thread({ withShell(app) { deauthorize(app, it) } }, "mibridge-cleanup").apply { isDaemon = true; start() }
            }
            return "mibridge: off"
        }
        if (Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "O3") return "mibridge: not on this SoC"
        val holds = parse(spec) ?: return "mibridge: cannot read '$spec'"
        worker?.interrupt()
        worker = Thread({ run(app, holds) }, "mibridge-boost").apply { isDaemon = true; start() }
        // Granted or not is known only later, from the worker: the MiBridgeBoost lines in the app's log.
        return "mibridge: asking for ${holds.joinToString { "${it.kind} level ${it.level}" }} (see the MiBridgeBoost lines for the outcome)"
    }

    /** At a session's end. */
    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker = null
    }

    /** Whether this thread is still the session's worker: stop() may have been missed in kadb's blocking I/O. */
    private fun current() = !Thread.currentThread().isInterrupted && worker === Thread.currentThread()

    private fun run(context: Context, holds: List<Hold>) {
        var authorized = false
        withShell(context) { authorized = authorize(context, it) }
        if (!authorized || !current()) return
        try {
            hold(context, holds)
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped", t)
        }
    }

    private fun withShell(context: Context, block: (com.flyfishxu.kadb.Kadb) -> Unit) {
        try {
            val adb = WirelessAdbFix.connectUsingSavedPairing(context)
            if (adb == null) {
                Log.i(TAG, "no wireless debugging pairing, or wireless debugging is off: nothing done")
                return
            }
            adb.use(block)
        } catch (_: InterruptedException) {
        } catch (t: Throwable) {
            Log.w(TAG, "stopped", t)
        }
    }

    private fun authorize(context: Context, adb: com.flyfishxu.kadb.Kadb): Boolean {
        val list = adb.shell("settings get system $SETTING").output.trim().let { if (it == "null") "" else it }
        val next = withPackage(list, context.packageName) ?: return true
        if (!PACKAGE_LIST.matches(next)) {
            Log.w(TAG, "unexpected $SETTING '$list': left as it is")
            return false
        }
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit().putBoolean(ADDED, true).commit()
        adb.shell("settings put system $SETTING '$next'")
        Log.i(TAG, "added to $SETTING")
        return true
    }

    private fun deauthorize(context: Context, adb: com.flyfishxu.kadb.Kadb) {
        val prefs = context.getSharedPreferences("session", Context.MODE_PRIVATE)
        if (!prefs.getBoolean(ADDED, false)) return
        val list = adb.shell("settings get system $SETTING").output.trim().let { if (it == "null") "" else it }
        val next = withoutPackage(list, context.packageName)
        if (next != null && !PACKAGE_LIST.matches(next)) {
            // Tried again at the next session.
            Log.w(TAG, "unexpected $SETTING '$list': this package left in it")
            return
        }
        if (next != null) {
            adb.shell(if (next.isEmpty()) "settings delete system $SETTING" else "settings put system $SETTING '$next'")
            Log.i(TAG, "taken out of $SETTING again")
        }
        prefs.edit().putBoolean(ADDED, false).commit()
    }

    private fun hold(context: Context, holds: List<Hold>) {
        val booster = PathClassLoader(JAR, ClassLoader.getSystemClassLoader())
            .loadClass("com.miui.performance.MiuiBooster").getConstructor().newInstance()
        val uid = Process.myUid()
        val check = booster.javaClass.getMethod("checkPermission", String::class.java, Int::class.javaPrimitiveType)
        // system_server reads the setting again on its own thread; give it a moment.
        fun granted(): Boolean {
            for (attempt in 0 until 10) {
                if (check.invoke(booster, context.packageName, uid) as Boolean) return true
                Thread.sleep(500)
            }
            return false
        }
        if (!granted()) {
            Log.w(TAG, "not granted by MirimThirdPartyPolicy")
            return
        }
        val int = Int::class.javaPrimitiveType
        val requests = holds.map { it to booster.javaClass.getMethod("request${it.method}HighFreq", int, int, int) }
        val cancels = holds.map { booster.javaClass.getMethod("cancel${it.method}HighFreq", int) }
        val renewAt = LongArray(holds.size)
        val last = IntArray(holds.size) { Int.MIN_VALUE }
        var regrantAt = 0L
        try {
            while (current()) {
                var now = SystemClock.elapsedRealtime()
                var notPermitted = false
                requests.forEachIndexed { i, (h, request: Method) ->
                    if (now < renewAt[i]) return@forEachIndexed
                    val r = request.invoke(booster, uid, h.level, h.timeoutMs) as Int
                    // 0 granted; -1 refused by system_server (a hot board for the CPU, the grant gone, or uniperf);
                    // -2 refused by MiuiBooster itself (its permission flag unset, or another uid)
                    if (r != last[i]) Log.i(TAG, "${h.kind} level ${h.level}: ${if (r == 0) "granted" else "refused ($r)"}")
                    last[i] = r
                    renewAt[i] = now + if (r == 0) h.renewMs else REFUSED_RETRY_MS
                    if (r == NOT_PERMITTED || (r != 0 && h.kind != "cpu")) notPermitted = true
                }
                // A grant taken away (a cloud update rewrote the list): listed and checked again, once a minute at most.
                if (notPermitted && now >= regrantAt) {
                    regrantAt = now + REGRANT_EVERY_MS
                    if (!(check.invoke(booster, context.packageName, uid) as Boolean)) {
                        var listed = false
                        withShell(context) { listed = authorize(context, it) }
                        val again = listed && current() && granted()
                        Log.i(TAG, "permission gone mid-session: " + if (again) "granted again" else "not granted again")
                        if (again) renewAt.fill(0L)
                    }
                    now = SystemClock.elapsedRealtime()
                }
                Thread.sleep(((renewAt.minOrNull() ?: now) - now).coerceIn(10L, POLL_MS))
            }
        } catch (_: InterruptedException) {
        } finally {
            cancels.forEach { runCatching { it.invoke(booster, uid) } }
        }
    }
}
