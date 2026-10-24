package com.droiddeck.launcher.session

import android.content.Context
import android.os.Build
import java.io.File

/**
 * [mali-g2] Per-thread QoS on the Xring O3, the one boost an app can give a thread there.
 *
 * Android's hint sessions reach nothing on this device: its power HAL (com.android.hardware.power.xring)
 * is AOSP's example service, which accepts sessions, hints and work durations and acts on none (it opens no
 * file and calls no other service; dumpsys performance_hint shows its 1 ms preferred rate and no headroom
 * support). HyperOS boosts threads with its own QoS levels instead, set through /dev/xr_fbt: libhwui and
 * libgui give their render threads level 9 with the RT type (SCHED_RR, priority 1) from inside every app,
 * and system_server gives the focused app's main and render threads the same while its window comes up and
 * the app's Mali driver threads level 6 with the VIP type. The kernel takes the request from any app for
 * threads of its own UID, so the game's threads in the guest can be given the same.
 *
 * [mode] is the session pref xringThreadQos: "vip" (6, VIP type), "rt" (9, RT type), or "<level>:<type>"
 * with an optional ":rtg" (the RTG bits libxqos never sets on this kernel) for experiments; anything else is off.
 */
internal class XrThreadQos private constructor(private val mode: Mode) {
    internal data class Mode(val name: String, val level: Int, val type: Int, val rtg: Boolean = false) {
        /** The ioctl's value: level in bits 0-3, scheduling type in 4-5, RTG in 6-7. */
        val value: Int get() = level or (type shl 4) or (if (rtg) 1 shl 6 else 0)
        override fun toString() = "$name (level $level, ${TYPES.getOrElse(type) { "type $type" }}${if (rtg) ", rtg" else ""})"
    }

    override fun toString() = mode.toString()

    /** Threads that were SCHED_OTHER before an RT level made them SCHED_RR. */
    private val raised = HashSet<Int>()
    private val refused = HashSet<Int>()

    /** Gives [tid] the level; returns a line for the log saying what the kernel made of it. */
    fun apply(tid: Int): String {
        val before = policy(tid)
        val result = XrQosNative.nativeSet(tid, mode.value)
        if (result < 0) {
            refused += tid
            return "xr qos $mode refused for $tid: errno ${-result}"
        }
        val after = policy(tid)
        if (mode.type == TYPE_RT && before != null && before and SCHED_MASK == SCHED_OTHER) raised += tid
        return "xr qos $mode on $tid: reads ${XrQosNative.nativeGet(tid).let { if (it < 0) "errno ${-it}" else "0x" + Integer.toHexString(it) }}" +
            ", policy ${describe(before)} -> ${describe(after)}"
    }

    /** Takes the level from [tid] again; null when the thread is gone or never had it. */
    fun clear(tid: Int): String? {
        if (refused.remove(tid)) return null
        val result = XrQosNative.nativeSet(tid, 0)
        val wasOther = raised.remove(tid)
        if (result < 0) return policy(tid)?.let { "xr qos clear failed for $tid: errno ${-result}" }
        // The kernel restores the policy it saw when the RT level was set; should it not, SCHED_OTHER by hand.
        val left = policy(tid) ?: return null
        if (wasOther && left and SCHED_MASK != SCHED_OTHER) {
            val back = XrQosNative.nativeSchedOther(tid)
            return "xr qos cleared on $tid, policy ${describe(left)} -> " + if (back < 0) "errno ${-back}" else describe(policy(tid))
        }
        return "xr qos cleared on $tid"
    }

    companion object {
        private const val TYPE_RT = 1
        private const val SCHED_OTHER = 0
        private const val SCHED_MASK = 0xff // without SCHED_RESET_ON_FORK, which the kernel adds to SCHED_RR
        private val TYPES = listOf("normal", "RT", "VIP")

        /** For a session on the O3 with the pref set, else null. */
        fun forSession(context: Context): XrThreadQos? {
            if (Build.SOC_MODEL != "O3") return null
            val mode = parse(SessionPrefs.xringThreadQos(context)) ?: return null
            XrQosNative.load()
            return XrThreadQos(mode)
        }

        /**
         * The pref's value as a mode, or null for off. Levels are those libxqos accepts besides 0: 3-10, level 6
         * only with the VIP type and 7-10 not with the normal type.
         */
        internal fun parse(value: String?): Mode? {
            val v = value?.trim()?.lowercase().orEmpty()
            when (v) {
                "vip" -> return Mode("vip", 6, 2)
                "rt" -> return Mode("rt", 9, TYPE_RT)
            }
            val parts = v.split(':')
            if (parts.size !in 2..3 || (parts.size == 3 && parts[2] != "rtg")) return null
            val level = parts[0].toIntOrNull() ?: return null
            val type = parts[1].toIntOrNull() ?: return null
            if (level !in 3..10 || type !in 0..2) return null
            if (level == 6 && type != 2 || level >= 7 && type == 0) return null
            return Mode(v, level, type, parts.size == 3)
        }

        /** The policy field of the thread's stat line (41st), or null when it is gone. /proc/<tid> works for any thread. */
        private fun policy(tid: Int): Int? = try {
            val line = File("/proc/$tid/stat").readText()
            line.substring(line.lastIndexOf(')') + 2).split(' ')[38].toInt()
        } catch (_: Exception) {
            null
        }

        private fun describe(policy: Int?): String = when (policy?.and(SCHED_MASK)) {
            null -> "?"
            SCHED_OTHER -> "normal"
            1 -> "FIFO"
            2 -> "RR"
            else -> policy.toString()
        }
    }
}

/** Loaded only when a mode is set, so the parsing above stays testable on the JVM. */
private object XrQosNative {
    fun load() = System.loadLibrary("sdnative")

    @JvmStatic external fun nativeSet(tid: Int, value: Int): Int
    @JvmStatic external fun nativeGet(tid: Int): Int
    @JvmStatic external fun nativeSchedOther(tid: Int): Int
}
