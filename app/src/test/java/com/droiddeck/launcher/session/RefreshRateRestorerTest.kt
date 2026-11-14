package com.droiddeck.launcher.session

import com.droiddeck.launcher.session.RefreshRateLiftLogic.Record
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/** [RefreshRateLiftLogic.armCommand]'s restorer run by this machine's sh, with a stand-in for Android's settings command. */
class RefreshRateRestorerTest {
    private lateinit var dir: File
    private val watched = ArrayList<Process>()

    @Before fun setUp() {
        assumeTrue(File("/bin/sh").canExecute() && File("/proc/self").isDirectory)
        assumeTrue(System.getenv("PATH").orEmpty().split(':').any { File(it, "setsid").canExecute() })
        dir = Files.createTempDirectory("refresh-lift").toFile()
        File(dir, "settings").apply {
            writeText(
                """
                #!/bin/sh
                f="${'$'}(dirname "${'$'}0")/value"
                case "${'$'}1 ${'$'}2 ${'$'}3" in
                "get secure user_refresh_rate") if [ -f "${'$'}f" ]; then cat "${'$'}f"; else echo null; fi ;;
                "put secure user_refresh_rate") echo "${'$'}4" > "${'$'}f" ;;
                "delete secure user_refresh_rate") rm -f "${'$'}f" ;;
                *) exit 1 ;;
                esac
                """.trimIndent() + "\n"
            )
            setExecutable(true)
        }
    }

    @After fun tearDown() {
        watched.forEach { it.destroy() }
        if (::dir.isInitialized) dir.deleteRecursively()
    }

    private val marker get() = File(dir, "marker")

    private var value: String?
        get() = File(dir, "value").takeIf(File::isFile)?.readText()?.trim()
        set(v) { if (v == null) File(dir, "value").delete() else File(dir, "value").writeText("$v\n") }

    private fun sh(line: String): Int {
        val builder = ProcessBuilder("/bin/sh", "-c", line).redirectErrorStream(true)
        builder.environment()["PATH"] = "${dir.path}:${System.getenv("PATH")}"
        return builder.start().waitFor()
    }

    /** A process standing in for the app's, and its restorer for [record]. */
    private fun armed(record: Record): Process {
        val app = ProcessBuilder("/bin/sh", "-c", "echo ${'$'}${'$'}; exec sleep 60").start().also { watched += it }
        val pid = app.inputStream.bufferedReader().readLine().trim().toInt()
        assertEquals(0, sh(RefreshRateLiftLogic.armCommand(marker.path, pid, record, pollSeconds = 1)))
        assertEquals(RefreshRateLiftLogic.markerLine(record), marker.readText().trim())
        return app
    }

    private fun gone(app: Process) {
        app.destroy()
        app.waitFor()
    }

    private fun waitFor(what: () -> Boolean) {
        val until = System.currentTimeMillis() + 10_000
        while (!what() && System.currentTimeMillis() < until) Thread.sleep(100)
    }

    @Test fun `once the process is gone the value before is put back`() {
        value = "144"
        val app = armed(Record("120", "144", "a1"))
        Thread.sleep(1_500)
        assertEquals("144", value)
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a1 120 144 restored", marker.readText().trim())
        assertEquals("120", value)
    }

    @Test fun `an unset value is deleted again`() {
        value = "144"
        val app = armed(Record(null, "144", "a2"))
        gone(app)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a2 unset 144 restored", marker.readText().trim())
        assertFalse(File(dir, "value").exists())
    }

    @Test fun `a value changed meanwhile is left`() {
        value = "144"
        val app = armed(Record("120", "144", "a3"))
        value = "90"
        gone(app)
        waitFor { marker.readText().trim().endsWith("left") }
        assertEquals("a3 120 144 left", marker.readText().trim())
        assertEquals("90", value)
    }

    @Test fun `put back by the app, the restorer stops and leaves a later 144 alone`() {
        value = "144"
        val app = armed(Record("120", "144", "a4"))
        assertEquals(0, sh(RefreshRateLiftLogic.putBackCommand(marker.path, "120")))
        assertEquals("120", value)
        assertFalse(marker.exists())
        value = "144"
        Thread.sleep(1_500)
        gone(app)
        Thread.sleep(2_500)
        assertEquals("144", value)
        assertFalse(marker.exists())
    }

    @Test fun `a later restorer replaces an earlier one`() {
        value = "144"
        val first = armed(Record("120", "144", "a5"))
        val second = armed(Record("60", "144", "a6"))
        gone(first)
        Thread.sleep(2_500)
        assertEquals("144", value)
        assertEquals("a6 60 144", marker.readText().trim())
        gone(second)
        waitFor { marker.readText().trim().endsWith("restored") }
        assertEquals("a6 60 144 restored", marker.readText().trim())
        assertEquals("60", value)
    }
}
