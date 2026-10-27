package com.droiddeck.launcher.session.play

import java.io.File
import java.io.RandomAccessFile

/**
 * [mali-g2] Reads the part of each log a play wrote, after the play: what went wrong, and a few facts
 * (swapchain size, renderer, DXVK version, Steam's exit code). Never on the render path.
 *
 * Each [Source] is read from its offset (resynced to the next line), at most [maxBytes] of it (the last
 * [maxBytes] when there is more), line by line with plain `contains` checks before any regex. Kept: per
 * kind of problem the count and the first [perKind] lines, [total] lines in all, each cut to [lineMax].
 */
class LogSliceScanner(
    private val maxBytes: Long = 16L * 1024 * 1024,
    private val perKind: Int = 3,
    private val total: Int = 20,
    private val lineMax: Int = 240,
) {
    class Source(val label: String, val file: File, val from: Long = 0L)

    class Kind(var n: Int = 0, val first: MutableList<String> = ArrayList())

    class Findings {
        val errors = LinkedHashMap<String, Kind>()
        var kept = 0
        /** Compositor [stats] lines: frames on screen, GPU frames from games, Vortek zero-copy frames, per 10 s. */
        val screenFrames = ArrayList<Int>()
        val gpuFrames = ArrayList<Int>()
        val zeroCopyFrames = ArrayList<Int>()
        /** vortek-x11 / hybris-x11 per-window fps reports (VORTEK_X11_STATS). */
        val x11Fps = ArrayList<Double>()
        /** Swapchain sizes seen, largest first by area at the end ([swapchains]). */
        private val swapchainSet = LinkedHashSet<String>()
        var dxvk: String? = null
        var renderer: String? = null
        /** "hybris" or "vortek", from droiddeck-game-env's line. */
        var gpuUsed: String? = null
        var steamWrapperPid: Int? = null
        var steamExitCode: Int? = null
        var steamEndedAt: String? = null
        var steers = 0
        var vortekExit: Int? = null
        val sources = ArrayList<String>()

        fun swapchain(size: String) { if (swapchainSet.size < 32) swapchainSet.add(size) }

        fun swapchains(): List<String> = swapchainSet.sortedByDescending { area(it) }

        fun has(kind: String) = (errors[kind]?.n ?: 0) > 0

        fun firstLine(kind: String): String? = errors[kind]?.first?.firstOrNull()

        private fun area(s: String): Long = s.split('x').mapNotNull { it.toLongOrNull() }.takeIf { it.size == 2 }?.let { it[0] * it[1] } ?: 0L
    }

    private class Pattern(val kind: String, val needle: String)

    fun scan(sources: List<Source>, appid: Long? = null): Findings = Findings().also { scanInto(it, sources, appid) }

    /** Adds [sources] to findings already made. */
    fun scanInto(f: Findings, sources: List<Source>, appid: Long? = null) {
        for (s in sources) {
            if (!s.file.isFile) continue
            f.sources.add(s.label)
            try {
                read(s) { line -> look(line, s.label, appid, f) }
            } catch (_: Exception) {
            }
        }
    }

    /** A finding that is not a log line (a crash report folder). */
    fun note(f: Findings, kind: String, line: String, label: String) = keep(f, kind, line, label)

    private fun read(s: Source, each: (String) -> Unit) {
        RandomAccessFile(s.file, "r").use { raf ->
            val length = raf.length()
            var from = s.from.coerceIn(0L, length)
            if (length - from > maxBytes) from = length - maxBytes
            raf.seek(from)
            val input = java.io.BufferedInputStream(java.io.FileInputStream(raf.fd), 64 * 1024)
            // Mid-line: start at the next line.
            if (from > 0) {
                raf.seek(from - 1)
                if (raf.read() != '\n'.code) {
                    while (true) { val b = input.read(); if (b < 0 || b == '\n'.code) break }
                }
            }
            val reader = java.io.BufferedReader(java.io.InputStreamReader(input, Charsets.UTF_8))
            var left = length - from
            while (left > 0) {
                val line = reader.readLine() ?: break
                left -= line.length + 1
                each(if (line.length > 4096) line.substring(0, 4096) else line)
            }
        }
    }

    private fun keep(f: Findings, kind: String, line: String, label: String) {
        val k = f.errors.getOrPut(kind) { Kind() }
        k.n++
        if (k.first.size < perKind && f.kept < total) {
            val text = "$label: ${line.trim()}"
            k.first.add(if (text.length > lineMax) text.substring(0, lineMax - 1) + "…" else text)
            f.kept++
        }
    }

    private fun look(line: String, label: String, appid: Long?, f: Findings) {
        // Facts first; a line is one or the other.
        if (facts(line, label, appid, f)) return
        for (p in PATTERNS) {
            if (line.contains(p.needle)) {
                keep(f, p.kind, line, label)
                return
            }
        }
        if (line.contains("now hidden, frames paced")) {
            // Small helper windows are hidden all the time; a game-sized one is worth noting.
            val size = WINDOW_SIZE.find(line)?.destructured?.let { (w, h) -> w.toLong() * h.toLong() } ?: 0L
            if (size >= 640L * 360L) keep(f, "hidden", line, label)
        }
    }

    private fun facts(line: String, label: String, appid: Long?, f: Findings): Boolean {
        when {
            line.contains("[stats] last 10 s:") -> {
                STATS_SCREEN.find(line)?.let { f.screenFrames.add(it.groupValues[1].toInt()) }
                STATS_GPU.find(line)?.let { f.gpuFrames.add(it.groupValues[1].toInt()) }
                STATS_ZERO.find(line)?.let { f.zeroCopyFrames.add(it.groupValues[1].toInt()) }
                return true
            }
            line.contains("-x11: ") && line.contains(" fps") -> {
                X11_FPS.find(line)?.let { f.x11Fps.add(it.groupValues[1].toDouble()); return true }
            }
            line.contains("swapchain") -> {
                (ZERO_COPY_SWAPCHAIN.find(line) ?: CLIENT_SWAPCHAIN.find(line))?.let {
                    val (w, h) = it.destructured
                    if (w.toInt() >= 320 && h.toInt() >= 200) f.swapchain("${w}x$h")
                    return true
                }
            }
            line.contains("info:  DXVK: v") -> { f.dxvk = line.substringAfter("DXVK: ").trim().take(60); return true }
            line.contains("Renderer:") && f.renderer == null -> { f.renderer = line.substringAfter("Renderer:").trim().take(80); return true }
            line.contains("droiddeck-game-env:") && line.contains("Vulkan in-process (libhybris)") -> { f.gpuUsed = "hybris"; return true }
            line.contains("droiddeck-game-env:") && line.contains("in-process GPU asked for, but") -> { f.gpuUsed = "vortek"; return true }
            line.contains("cluster steering: moved") -> { f.steers++; return true }
            line.contains("vortek-server exited: ") -> {
                f.vortekExit = line.substringAfter("vortek-server exited: ").trim().substringBefore(' ').toIntOrNull()
                keep(f, "vortek", line, label)
                return true
            }
            appid != null && line.contains("AppID $appid ") -> {
                if (f.steamWrapperPid == null) TRACKED.find(line)?.let {
                    if (line.contains("tracked process \"")) f.steamWrapperPid = it.groupValues[1].toInt()
                }
                val pid = f.steamWrapperPid
                if (pid != null && line.contains("no longer tracking PID $pid,")) {
                    f.steamExitCode = EXIT_CODE.find(line)?.groupValues?.get(1)?.toIntOrNull()
                    f.steamEndedAt = STAMP.find(line)?.groupValues?.get(1)
                }
                return true
            }
        }
        return false
    }

    companion object {
        private val PATTERNS = listOf(
            Pattern("crash", "Crash!!!"),
            Pattern("crash", "Unhandled exception"),
            Pattern("crash", "page fault on"),
            Pattern("crash", "EXCEPTION_ACCESS_VIOLATION"),
            Pattern("crash", "Fatal signal"),
            Pattern("device_lost", "VK_ERROR_DEVICE_LOST"),
            Pattern("device_lost", "DEVICE_LOST"),
            Pattern("device_lost", "Device lost"),
            Pattern("unsupported", "Skipping: Device does not support"),
            Pattern("unsupported", "No adapters found"),
            Pattern("unsupported", "failed to create factory"),
            Pattern("oom", "Scudo ERROR"),
            Pattern("oom", "Out of memory"),
            Pattern("oom", "OUT_OF_HOST_MEMORY"),
            Pattern("oom", "OUT_OF_DEVICE_MEMORY"),
            Pattern("oom", "bad_alloc"),
            Pattern("compiler", "cmpbe_v2_compile"),
            Pattern("vulkan", "err:vulkan"),
            Pattern("vulkan", "DxvkError"),
            Pattern("vulkan", "err:   "),
            Pattern("hidden", "zero-copy refused"),
            Pattern("module", "err:module"),
        )
        private val WINDOW_SIZE = Regex("""window 0x[0-9a-f]+ (\d+)x(\d+)""")
        private val STATS_SCREEN = Regex("""(\d+) frames on screen""")
        private val STATS_GPU = Regex("""(\d+) GPU frames from games""")
        private val STATS_ZERO = Regex("""(\d+) Vortek zero-copy frames""")
        private val X11_FPS = Regex("""-x11: \d+ frames, ([0-9.]+) fps""")
        private val ZERO_COPY_SWAPCHAIN = Regex("""zero-copy swapchain \d+ of presenter \d+: (\d+)x(\d+)""")
        private val CLIENT_SWAPCHAIN = Regex("""(?:vortek-x11|hybris-wsi): swapchain (\d+)x(\d+) (?:for window|format)""")
        private val TRACKED = Regex("""adding PID (\d+) as a tracked process""")
        private val EXIT_CODE = Regex("""exit code (-?\d+)""")
        private val STAMP = Regex("""^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})]""")
    }
}
