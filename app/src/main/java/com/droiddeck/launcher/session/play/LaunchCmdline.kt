package com.droiddeck.launcher.session.play

/**
 * [mali-g2] A Steam game launch, read from a process's argv.
 *
 * Steam starts every game as `steam-launch-wrapper -- reaper SteamLaunch AppId=<id> -- <tool> <verb> <exe> ...`
 * (gameprocess_log.txt shows the line). The wrapper and the reaper both carry it, so the play lasts while
 * either lives. With Proton the tool is the compatibility tool and the verb `waitforexitandrun`; Steam
 * also runs install scripts and the evaluator through the same tool with the `run` verb, which are not plays.
 * A native game has no verb: its binary follows the second `--`.
 */
object LaunchCmdline {
    class Launch(val appid: Long, /** Relative to steamapps/common when it is there. */ val exe: String?) {
        /** File name of the exe, for finding the game's own process. */
        val exeName: String? get() = exe?.substringAfterLast('/')?.takeIf { it.isNotEmpty() }
    }

    private val VERBS = setOf("waitforexitandrun", "run", "waitforexit", "getcompatpath", "getnativepath", "runinprefix", "destroyprefix")

    fun parse(argv: List<String>): Launch? {
        val launch = argv.indexOf("SteamLaunch")
        if (launch < 0) return null
        val appid = argv.getOrNull(launch + 1)?.takeIf { it.startsWith("AppId=") }
            ?.removePrefix("AppId=")?.toLongOrNull()?.takeIf { it > 0 } ?: return null
        val sep = argv.subList(launch, argv.size).indexOf("--").takeIf { it >= 0 }?.plus(launch) ?: return Launch(appid, null)
        val rest = argv.subList(sep + 1, argv.size)
        val verbAt = rest.indexOfFirst { it in VERBS }
        val exe = when {
            verbAt >= 0 && rest[verbAt] != "waitforexitandrun" -> return null
            verbAt >= 0 -> rest.drop(verbAt + 1).firstOrNull { it.isNotBlank() }
            else -> rest.firstOrNull { it.isNotBlank() }
        }
        return Launch(appid, exe?.let(::relative))
    }

    /** The exe as the journal stores it: below steamapps/common, else its last two path parts. */
    fun relative(path: String): String {
        val p = path.trim().trim('\'', '"').replace('\\', '/')
        val common = p.indexOf("/steamapps/common/")
        if (common >= 0) return p.substring(common + "/steamapps/common/".length)
        val parts = p.split('/').filter { it.isNotEmpty() }
        return parts.takeLast(2).joinToString("/")
    }

    /** Whether a process whose argv[0] is [argv0] runs [exeName] (wine shows the Windows path). */
    fun runs(argv0: String, exeName: String): Boolean {
        if (exeName.isEmpty()) return false
        val base = argv0.replace('\\', '/').substringAfterLast('/')
        return base.equals(exeName, ignoreCase = true)
    }
}
