package com.droiddeck.launcher.session.play

/**
 * [mali-g2] Skin and SoC temperatures and PowerKeeper's refresh cap for the play journal, read through the
 * wireless debugging connection HyperOsGameMode keeps open during a session (the app itself sees no thermal
 * zones). [tick] is called from that loop every few seconds; it runs the command only while a play is open
 * ([wanted]) and at most every [EVERY_MS]. Without a pairing nothing runs and the journal falls back to the
 * battery temperature and Android's thermal status.
 */
object ShellProbe {
    const val EVERY_MS = 30_000L
    const val COMMAND = "dumpsys thermalservice | grep -E 'Temperature\\{|emperatures'; " +
        "echo \"LIMIT \$(settings get system thermal_limit_refresh_rate)\""

    class Reading(
        val atMs: Long,
        /** quiet_therm (AOSP type 3, skin), else a sensor named like the shell. */
        val skinC: Float?,
        /** Hottest CPU or GPU sensor (types 0 and 1). */
        val socC: Float?,
        val batteryC: Float?,
        /** PowerKeeper's thermal_limit_refresh_rate; 0 is no cap. */
        val limitHz: Int?,
    )

    @Volatile var wanted = false
    @Volatile var last: Reading? = null
    @Volatile private var lastRunMs = 0L

    /** From HyperOsGameMode's loop; [shell] runs a command on the connection and returns its output. */
    fun tick(nowMs: Long, shell: (String) -> String) {
        if (!wanted || nowMs - lastRunMs < EVERY_MS) return
        lastRunMs = nowMs
        last = try { parse(shell(COMMAND), nowMs) } catch (_: Exception) { null }
    }

    fun reset() {
        last = null
        lastRunMs = 0L
    }

    private val TEMP = Regex("""Temperature\{mValue=([-+0-9.eE]+|NaN), mType=(-?\d+), mName=([^,}]+)""")

    /**
     * dumpsys thermalservice's `Temperature{...}` entries: the "Current temperatures from HAL" section wins over
     * the cached one for a sensor it lists (that section often holds only some).
     */
    fun parse(text: String, atMs: Long): Reading {
        val cached = LinkedHashMap<String, Pair<Int, Float>>()
        val current = LinkedHashMap<String, Pair<Int, Float>>()
        var section = cached
        var limit: Int? = null
        for (line in text.lineSequence()) {
            if (line.contains("emperatures") && !line.contains("{")) {
                section = if (line.contains("Current")) current else cached
                continue
            }
            if (line.startsWith("LIMIT ")) {
                limit = line.removePrefix("LIMIT ").trim().toFloatOrNull()?.toInt()
                continue
            }
            val m = TEMP.find(line) ?: continue
            val v = m.groupValues[1].toFloatOrNull() ?: continue
            if (v.isNaN() || v < -100f || v > 200f) continue
            section[m.groupValues[3].trim()] = m.groupValues[2].toInt() to v
        }
        val all = LinkedHashMap(cached).apply { putAll(current) }
        val skin = all.filter { it.value.first == 3 }.values.maxOfOrNull { it.second }
            ?: all.filter { it.key.contains("shell") }.values.maxOfOrNull { it.second }
        val soc = all.filter { (it.value.first == 0 || it.value.first == 1) && it.value.second > 0f }.values.maxOfOrNull { it.second }
        val battery = all.filter { it.value.first == 2 || it.key == "battery" }.values.maxOfOrNull { it.second }
        return Reading(atMs, skin, soc, battery, limit)
    }
}
