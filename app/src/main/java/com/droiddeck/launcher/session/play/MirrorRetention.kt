package com.droiddeck.launcher.session.play

import java.io.File

/**
 * [mali-g2] The `.keep` marker of a session folder: `until=<epoch ms> why=crash,marked`, written by the play
 * journal into the session folder before it is collected and mirrored, so it travels with the mirror.
 * Writers only ever extend it: the latest end and every reason.
 */
object KeepMarker {
    const val NAME = ".keep"

    class Keep(val untilMs: Long, val why: Set<String>)

    fun read(dir: File): Keep? = try {
        val f = File(dir, NAME)
        if (!f.isFile) null else parse(f.readText())
    } catch (_: Exception) {
        null
    }

    fun parse(text: String): Keep? {
        var until: Long? = null
        val why = LinkedHashSet<String>()
        for (part in text.trim().split(Regex("\\s+"))) {
            when {
                part.startsWith("until=") -> until = part.removePrefix("until=").toLongOrNull()
                part.startsWith("why=") -> part.removePrefix("why=").split(',').filter { it.isNotBlank() }.forEach { why += it }
            }
        }
        return until?.let { Keep(it, why) }
    }

    @Synchronized
    fun extend(dir: File, untilMs: Long, why: Collection<String>) {
        if (!dir.isDirectory) return
        val old = read(dir)
        val until = maxOf(untilMs, old?.untilMs ?: 0L)
        val reasons = LinkedHashSet<String>().apply { old?.why?.let { addAll(it) }; addAll(why) }
        try {
            val tmp = File(dir, "$NAME.tmp")
            tmp.writeText("until=$until why=${reasons.joinToString(",")}\n")
            if (!tmp.renameTo(File(dir, NAME))) { File(dir, NAME).writeText(tmp.readText()); tmp.delete() }
        } catch (_: Exception) {
        }
    }
}

/**
 * [mali-g2] Which mirrored session folders (getExternalFilesDir()/logs) to keep, slim or delete.
 *
 * A Steam session folder is 43-80 MB, 95 % of it Steam's own logs that go back days (the .previous files,
 * webhelper and cef logs); cut to the session's time window ([SteamLogSlim]) the same folder is about 7 MB.
 * - The newest [FULL] finished folders are kept as they are.
 * - Up to the newest [KEEP] are kept, slimmed: about two days of sessions.
 * - Older ones only while their `.keep` says so (a crash, a freeze... 14 days; low fps 7; marked by the owner 30).
 * - In all at most [CAP] bytes ([LOW_CAP] when the shared storage has under [LOW_FREE] free). Over it, the
 *   oldest go first: unmarked ones past the newest [FULL], then flagged ones, then the rest of the newest
 *   [FULL], then the ones the owner marked. The newest folder, the running session's and one still being
 *   collected are never deleted. Typical total: 3 x 80 + 7 x 7 + some 20 flagged x 7 MB, about 430 MB.
 */
object MirrorRetention {
    const val FULL = 3
    const val KEEP = 10
    const val CAP = 1L shl 30
    const val LOW_FREE = 2L shl 30
    const val LOW_CAP = 256L shl 20

    class Folder(val name: String, val bytes: Long, val keep: KeepMarker.Keep?, val slimmed: Boolean)

    class Plan(val slim: List<String>, val delete: List<String>)

    /**
     * [folders] oldest first. [protect]ed names are neither slimmed nor deleted. [bytesAfterSlim] estimates a
     * folder's size once slimmed (the caller slims before it deletes, and plans again with real sizes).
     */
    fun plan(
        folders: List<Folder>, nowMs: Long, freeBytes: Long, protect: Set<String>,
        bytesAfterSlim: (Folder) -> Long = { it.bytes },
    ): Plan {
        val newestFirst = folders.reversed()
        val finished = newestFirst.filter { it.name !in protect }
        val rank = HashMap<String, Int>().apply { finished.forEachIndexed { i, f -> put(f.name, i) } }
        val slim = ArrayList<String>()
        val delete = LinkedHashSet<String>()
        fun kept(f: Folder) = f.keep != null && f.keep.untilMs > nowMs
        for (f in finished) {
            val r = rank.getValue(f.name)
            when {
                r < FULL -> {}
                r < KEEP || kept(f) -> if (!f.slimmed) slim += f.name
                else -> delete += f.name
            }
        }
        val cap = if (freeBytes in 0 until LOW_FREE) LOW_CAP else CAP
        var total = newestFirst.filter { it.name !in delete }.sumOf { if (it.name in slim) bytesAfterSlim(it) else it.bytes }
        if (total > cap) {
            val oldestFirst = finished.reversed().filter { it.name !in delete && rank.getValue(it.name) > 0 }
            fun marked(f: Folder) = kept(f) && "marked" in f.keep!!.why
            val order = oldestFirst.filter { rank.getValue(it.name) >= FULL && !kept(it) } +
                oldestFirst.filter { kept(it) && !marked(it) } +
                oldestFirst.filter { rank.getValue(it.name) < FULL && !kept(it) } +
                oldestFirst.filter { marked(it) }
            for (f in order) {
                if (total <= cap) break
                delete += f.name
                total -= if (f.name in slim) bytesAfterSlim(f) else f.bytes
            }
        }
        return Plan(slim.filter { it !in delete }, delete.toList())
    }

    fun size(dir: File): Long = dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    /**
     * Applies the plan to [base] (the mirror's logs folder). [order] sorts session folders oldest first
     * (SessionPaths.chronological), [isSessionFolder] picks them out. Returns a line for the log.
     */
    fun apply(
        base: File, nowMs: Long, freeBytes: Long, protect: Set<String>,
        order: Comparator<File>, isSessionFolder: (File) -> Boolean,
        zone: java.util.TimeZone = java.util.TimeZone.getDefault(),
    ): String {
        fun folders() = (base.listFiles { f -> isSessionFolder(f) }?.toList() ?: emptyList()).sortedWith(order)
            .map { Folder(it.name, size(it), KeepMarker.read(it), File(it, SteamLogSlim.MARKER).exists()) }
        val first = plan(folders(), nowMs, freeBytes, protect)
        var slimmed = 0L
        for (name in first.slim) slimmed += SteamLogSlim.slim(File(base, name), zone)
        // Plan again with the real sizes; the slimming is done, only the deletions are new.
        val plan = if (first.slim.isEmpty()) first else plan(folders(), nowMs, freeBytes, protect)
        for (name in plan.delete) File(base, name).deleteRecursively()
        return "mirror: ${first.slim.size} folder(s) slimmed (${slimmed shr 20} MB freed), ${plan.delete.size} deleted"
    }
}
