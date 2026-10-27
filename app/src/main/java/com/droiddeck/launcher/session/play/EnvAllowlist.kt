package com.droiddeck.launcher.session.play

/**
 * [mali-g2] The game environment variables the play journal records: the switches that change how a game
 * runs (DXVK, VKD3D, Proton, Vortek, Wine sync, FEX, Mesa), nothing else. An allowlist, so a Steam user
 * name, a token or a home path in some other variable never gets in.
 */
object EnvAllowlist {
    private val PREFIXES = listOf(
        "DXVK_", "VKD3D_", "PROTON_", "VORTEK_", "DROIDDECK_", "BL_SYNC", "BL_GPU_MODE", "FEX_", "MESA_", "ZINK_",
        "GALLIUM_", "TU_", "WINE_", "HYBRIS_",
    )
    private val EXACT = setOf("WINEESYNC", "WINEFSYNC", "WINENTSYNC", "WINEDEBUG", "WINEDLLOVERRIDES", "LD_PRELOAD", "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES", "VK_INSTANCE_LAYERS", "SteamAppId")
    private val PATH_LISTS = setOf("LD_PRELOAD", "VK_ICD_FILENAMES", "VK_DRIVER_FILES")
    const val MAX_KEYS = 60
    const val MAX_VALUE = 160

    fun allowed(key: String): Boolean = key in EXACT || PREFIXES.any { key.startsWith(it) }

    fun filter(environ: List<String>): Map<String, String> {
        val out = java.util.TreeMap<String, String>()
        for (entry in environ) {
            val eq = entry.indexOf('=')
            if (eq <= 0) continue
            val key = entry.substring(0, eq)
            if (!allowed(key)) continue
            var value = entry.substring(eq + 1)
            // Paths say where things are installed; the file names say what was loaded.
            if (key in PATH_LISTS) value = value.split(':', ' ').filter { it.isNotEmpty() }.joinToString(",") { it.substringAfterLast('/') }
            if (value.length > MAX_VALUE) value = value.substring(0, MAX_VALUE - 1) + "…"
            out[key] = value
        }
        return if (out.size <= MAX_KEYS) out else out.entries.take(MAX_KEYS).associateTo(LinkedHashMap()) { it.key to it.value }
    }
}
