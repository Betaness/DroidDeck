package com.droiddeck.launcher.session

import android.content.Context
import android.util.Log
import com.droiddeck.launcher.core.DeviceSupport
import com.droiddeck.launcher.core.HostProcess
import com.droiddeck.launcher.core.SessionPart
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter

/**
 * [mali-g2] Vortek Vulkan forwarding for devices without Adreno/KGSL (Mali).
 *
 * The guest has no usable GPU node, so its Vulkan ICD (libvulkan_vortek.so, staged into the
 * rootfs by [SessionFiles]) forwards every call over a unix socket to this server, a bionic
 * executable shipped as lib/arm64-v8a/libvortekserver.so that loads the vendor Vulkan HAL
 * through the exported "sphal" linker namespace. It must run from /data (nativeLibraryDir is),
 * where the linker config gives it that namespace. Sources and build: mali-g2/vortek.
 *
 * The socket is filesDir/.wayland-rt/vortek/V0, which the guest sees as [GUEST_SOCKET].
 */
class VortekServerComponent(
    private val socket: File,
    private val logFile: File?,
    private val extraEnv: List<String>,
) : SessionPart() {
    @Volatile private var pid = -1

    @Volatile private var stopping = false
    @Volatile private var generation = 0
    private val crashes = ArrayDeque<Long>()

    override fun start() {
        stop()
        stopping = false
        synchronized(crashes) { crashes.clear() }
        launch()
    }

    private fun launch() {
        val binary = binary(app())
        val hal = halDriver()
        if (!binary.isFile || hal == null) {
            Log.w(TAG, "not starting: server ${binary.isFile}, HAL ${hal?.path}")
            return
        }
        socket.parentFile?.mkdirs()
        socket.delete()
        val env = ArrayList<String>()
        env.add("VORTEK_HAL_DRIVER=" + hal.path)
        env.add("VORTEK_PRESENT_LOG=0")
        // The compositor listens there (waylandcomp/src/vortek_present.c) for zero-copy presents.
        socket.parentFile?.parentFile?.let { env.add("VORTEK_PRESENT_SOCKET=" + File(it, "vortek-present").path) }
        // Compiled pipelines per game and driver build (mali-g2/vortek/src/pipeline_cache.c); in the
        // cache dir, which Android may clear when storage runs low. droiddeck-env can set another
        // directory, or VORTEK_PIPELINE_CACHE=0 to turn it off.
        if (extraEnv.none { it.startsWith("VORTEK_PIPELINE_CACHE_DIR=") }) {
            env.add("VORTEK_PIPELINE_CACHE_DIR=" + File(app().cacheDir, PIPELINE_CACHE_DIR).path)
        }
        env.addAll(extraEnv)
        val log = logFile?.let {
            runCatching { PrintWriter(FileWriter(it, true)) }.onFailure { e -> Log.w(TAG, "could not open $it", e) }.getOrNull()
        }
        val mine = ++generation
        log?.apply { println("== vortek-server starting: ${binary.path} ${socket.path} (HAL ${hal.path})"); flush() }
        val command = listOf(binary.absolutePath, socket.absolutePath)
            .joinToString(" ") { it.replace("\\", "\\\\").replace(" ", "\\ ") }
        pid = HostProcess.start(command, env.toTypedArray(), app().filesDir, { status ->
            Log.i(TAG, "vortek-server exited: $status")
            log?.let { synchronized(it) { it.println("== vortek-server exited: $status"); it.flush() } }
            if (mine == generation) restartAfterCrash()
        }) { line ->
            Log.i(TAG, line)
            log?.let { synchronized(it) { it.println(line); it.flush() } }
        }
        Log.i(TAG, "started pid=$pid socket=$socket hal=$hal")
    }

    // Every guest Vulkan client loses its device when the server dies; processes started after a
    // restart (Steam relaunches its web helper) get a working one again.
    private fun restartAfterCrash() {
        if (stopping) return
        val now = android.os.SystemClock.elapsedRealtime()
        val recent = synchronized(crashes) {
            crashes.addLast(now)
            while (crashes.isNotEmpty() && now - crashes.first() > RESTART_WINDOW_MS) crashes.removeFirst()
            crashes.size
        }
        if (recent > MAX_RESTARTS) {
            Log.w(TAG, "vortek-server crashed $recent times within ${RESTART_WINDOW_MS / 1000} s; not restarting")
            return
        }
        val crashed = generation
        Thread({
            Thread.sleep(500)
            if (!stopping && crashed == generation) launch()
        }, "vortek-restart").start()
    }

    override fun stop() {
        stopping = true
        generation++
        if (pid > 1) android.os.Process.killProcess(pid)
        pid = -1
    }

    companion object {
        private const val TAG = "VortekServer"
        const val BINARY = "libvortekserver.so"
        const val MODE = "vortek"
        /** filesDir/.wayland-rt is bound to LinuxRuntime.GUEST_RUNTIME_DIR (/run/droiddeck) in the guest. */
        const val GUEST_SOCKET = "/run/droiddeck/vortek/V0"
        const val GUEST_DIR = "/usr/local/lib/droiddeck-vortek"
        const val GUEST_ICD = "$GUEST_DIR/vortek_icd.aarch64.json"
        private const val HAL_DIR = "/vendor/lib64/hw"
        private const val PIPELINE_CACHE_DIR = "vortek-pipelines"
        private const val MAX_RESTARTS = 5
        private const val RESTART_WINDOW_MS = 60_000L

        fun binary(context: Context): File = File(context.applicationInfo.nativeLibraryDir, BINARY)

        /** The vendor Vulkan HAL module: vulkan.mali.so, else any non-Adreno vulkan.*.so. */
        fun halDriver(): File? {
            File(HAL_DIR, "vulkan.mali.so").takeIf { it.isFile }?.let { return it }
            return File(HAL_DIR).listFiles { f -> f.name.startsWith("vulkan.") && f.name.endsWith(".so") && !f.name.contains("adreno") }
                ?.minByOrNull { it.name }
        }

        /** True on a device this build forwards Vulkan for: no Adreno, a vendor HAL and the server in the APK. */
        fun available(context: Context): Boolean =
            !DeviceSupport.adreno() && halDriver() != null && binary(context).isFile
    }
}
