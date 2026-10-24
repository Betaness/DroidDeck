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
 * session's start, and taken out again at the first session after the setting here is cleared. Being listed
 * only lets the app ask. The floors are system-wide clock requests, so the game in the session's child
 * processes gets them too; they lapse by themselves when the app stops renewing them.
 *
 * The setting [SessionPrefs.miBridgeBoost] names what to hold, e.g. "gpu=1" or "gpu=1,ddr=1,cpu=1";
 * empty is off. Which clocks the levels map to is not known yet: uniperf's tables are not readable.
 */
object MiBridgeBoost {
    private const val TAG = "MiBridgeBoost"
    private const val SETTING = "mibridge_authorized_pkg_list"
    private const val JAR = "/system_ext/framework/MiuiBooster.jar"
    private const val ADDED = "miBridgeAuthorized" // session prefs: this app put itself in SETTING
    private const val RENEW_EARLY_MS = 300L
    private val MAX_TIMEOUT_MS = intArrayOf(0, 10_000, 5_000, 1_500)
    private val PACKAGE_LIST = Regex("[A-Za-z0-9._,]*")

    internal data class Hold(val kind: String, val level: Int) {
        val method get() = kind.replaceFirstChar { it.uppercaseChar() }
        val timeoutMs get() = MAX_TIMEOUT_MS[level]
    }

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
        return "mibridge: holding ${holds.joinToString { "${it.kind} level ${it.level}" }}"
    }

    /** At a session's end. */
    @Synchronized
    fun stop() {
        worker?.interrupt()
        worker = null
    }

    private fun run(context: Context, holds: List<Hold>) {
        var authorized = false
        withShell(context) { authorized = authorize(context, it) }
        if (!authorized) return
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
        if (next != null && PACKAGE_LIST.matches(next)) {
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
        var granted = false
        for (attempt in 0 until 10) {
            granted = check.invoke(booster, context.packageName, uid) as Boolean
            if (granted) break
            Thread.sleep(500)
        }
        if (!granted) {
            Log.w(TAG, "not granted by MirimThirdPartyPolicy")
            return
        }
        val int = Int::class.javaPrimitiveType
        val requests = holds.map { it to booster.javaClass.getMethod("request${it.method}HighFreq", int, int, int) }
        val cancels = holds.map { booster.javaClass.getMethod("cancel${it.method}HighFreq", int) }
        val renewAt = LongArray(holds.size)
        val last = IntArray(holds.size) { Int.MIN_VALUE }
        try {
            while (!Thread.currentThread().isInterrupted) {
                val now = SystemClock.elapsedRealtime()
                requests.forEachIndexed { i, (h, request: Method) ->
                    if (now < renewAt[i]) return@forEachIndexed
                    val r = request.invoke(booster, uid, h.level, h.timeoutMs) as Int
                    // 0 granted; -1 refused (a hot board for the CPU, or uniperf); -2 no answer
                    if (r != last[i]) Log.i(TAG, "${h.kind} level ${h.level}: ${if (r == 0) "granted" else "refused ($r)"}")
                    last[i] = r
                    renewAt[i] = now + if (r == 0) h.timeoutMs - RENEW_EARLY_MS else 5_000L
                }
                Thread.sleep(250)
            }
        } catch (_: InterruptedException) {
        } finally {
            cancels.forEach { runCatching { it.invoke(booster, uid) } }
        }
    }
}
