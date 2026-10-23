package com.droiddeck.launcher.session

import android.util.Log
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * [mali-g2] The game's threads moved off a CPU cluster that HyperOS holds at its lowest clock.
 *
 * On Xiaomi's Xring O3, HyperOS hands the session to SchedGenius's game frame regulator: libmigui's
 * GameRenderDetector reports the launcher's own render thread as the game's, whatever the manifest
 * says. The regulator sizes the clocks by that process's threads, which are idle while the game runs in
 * child processes. It held the mid cores (4-7), where the scheduler put the game, at 0.56-0.84 GHz
 * with Duckov's main thread busy 95% on one. A busy loop on another mid core could not raise them
 * either, and no thread of this UID was placed on the prime cores. The little cores (0-3) were not held,
 * and ran up to 3.1 GHz. Duckov at 1704x1136 ran 27 fps there with its threads on the mid cores,
 * 47 fps with its main thread on 0-3, and 69 fps with all its threads and the Vortek server on 0-3.
 *
 * Once a second, the busiest thread of the session is checked. It counts when it is busy above [BUSY] on a
 * cluster whose clock stays in the bottom [HELD_SHARE] of its range: a governor raises the clock under
 * such a thread. After [HELD_S] such seconds, every thread of the session's busiest processes moves to
 * the lowest-capacity cluster. Threads started later inherit their creator's cores; a process that
 * joins the busiest is moved too. Every [RETRY_S] seconds they get every core back, in case the regulator
 * let go; if the cluster is still held, they move again [HELD_S] seconds later.
 */
internal class ClusterSteer(private val clusters: List<Cluster>) {
    internal data class Cluster(val cpus: List<Int>, val policy: File, val capacity: Int) {
        val mask: Long get() = cpus.fold(0L) { m, cpu -> m or (1L shl cpu) }
    }

    /** What to do after a second: nothing, move the threads, or give them every core back. */
    internal enum class Action { NONE, STEER, RELEASE }

    /** The decision alone, without /proc or /sys, so it can be tested. */
    internal class Decider {
        var steered = false
            private set
        private var heldFor = 0
        private var steeredFor = 0

        fun tick(busyOnHeldCluster: Boolean): Action {
            if (steered) {
                if (++steeredFor < RETRY_S) return Action.NONE
                steered = false
                heldFor = 0
                return Action.RELEASE
            }
            heldFor = if (busyOnHeldCluster) heldFor + 1 else 0
            if (heldFor < HELD_S) return Action.NONE
            steered = true
            steeredFor = 0
            return Action.STEER
        }
    }

    private val decider = Decider()
    private val all = (1L shl Runtime.getRuntime().availableProcessors()) - 1
    private val target = clusters.minByOrNull { it.capacity }
    private val moved = HashMap<Int, Int>() // pid -> its thread count when last moved

    /**
     * One second: [top] are the session's busiest processes, [busiest] its busiest thread (pid, tid)
     * and that thread's busy share. Returns a line for the log when something changed.
     */
    fun tick(top: List<Int>, busiest: Triple<Int, Int, Double>?): String? {
        val target = target ?: return null
        val cluster = busiest?.let { (pid, tid, _) -> cpuOf(pid, tid)?.let { cpu -> clusters.firstOrNull { cpu in it.cpus } } }
        val held = cluster != null && cluster != target && busiest.third >= BUSY && isHeld(cluster)
        return when (decider.tick(held)) {
            Action.STEER -> {
                top.forEach { move(it, target.mask) }
                "moved ${top.size} processes to cores ${target.cpus.first()}-${target.cpus.last()}: " +
                    "cores ${cluster!!.cpus.first()}-${cluster.cpus.last()} held at ${khz(cluster.policy, "scaling_cur_freq")} kHz " +
                    "under a thread ${(busiest!!.third * 100).toInt()}% busy"
            }
            Action.RELEASE -> {
                release()
                "every core back, to see whether the clock is still held"
            }
            Action.NONE -> {
                if (decider.steered) top.forEach { pid -> if (threadCount(pid) != moved[pid]) move(pid, target.mask) }
                null
            }
        }
    }

    /** At the session's end, or before a retry: every core for the threads that were moved. */
    fun release() {
        moved.keys.forEach { taskset(all, it) }
        moved.clear()
    }

    private fun move(pid: Int, mask: Long) {
        if (taskset(mask, pid)) moved[pid] = threadCount(pid) else moved.remove(pid)
    }

    private fun isHeld(c: Cluster): Boolean {
        val cur = khz(c.policy, "scaling_cur_freq") ?: return false
        val min = khz(c.policy, "scaling_min_freq") ?: return false
        val max = khz(c.policy, "cpuinfo_max_freq") ?: return false
        return held(cur, min, max)
    }

    companion object {
        private const val TAG = "ClusterSteer"
        internal const val BUSY = 0.6
        internal const val HELD_SHARE = 0.25
        internal const val HELD_S = 3
        internal const val RETRY_S = 60

        /** A clock in the bottom [HELD_SHARE] of the cluster's range. */
        internal fun held(curKhz: Long, minKhz: Long, maxKhz: Long): Boolean =
            maxKhz > minKhz && curKhz <= minKhz + (maxKhz - minKhz) * HELD_SHARE

        /** The device's clusters from cpufreq, or null when fewer than two can be told apart. */
        fun forDevice(): ClusterSteer? {
            val clusters = File("/sys/devices/system/cpu/cpufreq").listFiles { f -> f.name.startsWith("policy") }
                ?.mapNotNull { policy ->
                    val cpus = runCatching { File(policy, "related_cpus").readText().trim().split(' ').map { it.toInt() } }.getOrNull()
                    val capacity = cpus?.firstOrNull()?.let { readLong("/sys/devices/system/cpu/cpu$it/cpu_capacity") }
                    if (cpus.isNullOrEmpty() || capacity == null) null else Cluster(cpus, policy, capacity.toInt())
                }
                ?.sortedBy { it.cpus.first() }
                .orEmpty()
            return if (clusters.map { it.capacity }.distinct().size >= 2) ClusterSteer(clusters) else null
        }

        private fun khz(policy: File, name: String) = readLong(File(policy, name).path)

        private fun readLong(path: String): Long? = try { File(path).readText().trim().toLong() } catch (_: Exception) { null }

        private fun threadCount(pid: Int) = File("/proc/$pid/task").list()?.size ?: 0

        /** The CPU a thread last ran on: field 39 of its stat line. */
        private fun cpuOf(pid: Int, tid: Int): Int? = try {
            val line = File("/proc/$pid/task/$tid/stat").readText()
            line.substring(line.lastIndexOf(')') + 2).split(' ')[36].toInt()
        } catch (_: Exception) {
            null
        }

        /** toybox taskset on every thread of [pid]. */
        private fun taskset(mask: Long, pid: Int): Boolean {
            var p: Process? = null
            return try {
                p = ProcessBuilder("/system/bin/taskset", "-ap", java.lang.Long.toHexString(mask), pid.toString())
                    .redirectErrorStream(true).start()
                p.inputStream.readBytes()
                p.waitFor(5, TimeUnit.SECONDS) && p.exitValue() == 0
            } catch (e: InterruptedException) {
                Thread.currentThread().interrupt()
                false
            } catch (e: Exception) {
                Log.w(TAG, "taskset ${java.lang.Long.toHexString(mask)} $pid", e)
                false
            } finally {
                p?.destroy()
            }
        }
    }
}
