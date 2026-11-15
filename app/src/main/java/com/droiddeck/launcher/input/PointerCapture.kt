package com.droiddeck.launcher.input

/**
 * When the session wants Android's pointer capture for the mouse.
 *
 * Captured, the mouse arrives as raw counts. Uncaptured, it arrives as pointer positions with
 * Android's pointer acceleration applied (AXIS_RELATIVE_X/Y carry the same accelerated motion),
 * clamped at the screen edges.
 */
object PointerCapturePolicy {
    /** Why capture is not wanted, or null when it is. */
    fun blocker(
        mouseAttached: Boolean,
        pointerLocked: Boolean,
        multiWindow: Boolean,
        glassesOutput: Boolean,
        resumed: Boolean,
        windowFocused: Boolean,
        drawerOpen: Boolean,
        pcKeyboardOpen: Boolean,
        achievementsOpen: Boolean,
        inputSettingsOpen: Boolean,
        pipUi: Boolean,
        bottomOutput: Boolean,
        touchEditorOpen: Boolean,
    ): String? = when {
        !resumed -> "paused"
        !windowFocused -> "no window focus"
        pipUi -> "picture-in-picture"
        drawerOpen -> "drawer open"
        pcKeyboardOpen -> "PC keyboard open"
        achievementsOpen -> "achievements open"
        inputSettingsOpen -> "input settings open"
        touchEditorOpen -> "touch editor open"
        bottomOutput -> "game on the bottom screen"
        !mouseAttached -> "no mouse"
        !pointerLocked && multiWindow -> "multi-window without pointer lock"
        !pointerLocked && glassesOutput -> "glasses output without pointer lock"
        else -> null
    }
}

/**
 * Handles hardware mouse events that arrive uncaptured while the session wants capture.
 *
 * Capture is requested again at once and then at most every [retryMs]. Motion is held back for up to
 * [holdMs], so accelerated positions do not reach the game; if capture is still off by then, motion
 * passes through so the mouse keeps working, and requests slow to every [slowRetryMs].
 * [captureChanged] starts over.
 */
class UncapturedMouseGuard(
    private val retryMs: Long = RETRY_MS,
    private val holdMs: Long = HOLD_MS,
    private val slowRetryMs: Long = SLOW_RETRY_MS,
) {
    class Verdict(
        val requestCapture: Boolean,
        val holdMotion: Boolean,
        /** First uncaptured event while capture is wanted since capture last changed. */
        val first: Boolean,
        /** Capture stayed off for [holdMs]; motion passes and requests slow down from now on. */
        val gaveUp: Boolean,
    )

    private var requested = false
    private var requestedAt = 0L
    private var seen = false
    private var since = -1L
    private var passing = false

    /** Mouse events whose motion was held since capture last changed. */
    var held = 0
        private set

    fun mayRequest(now: Long): Boolean =
        !requested || now - requestedAt >= (if (passing) slowRetryMs else retryMs)

    fun requested(now: Long) {
        requested = true
        requestedAt = now
    }

    fun captureChanged() {
        seen = false
        since = -1L
        passing = false
        held = 0
    }

    /** [holdable] is false where uncaptured motion has to keep flowing (the desktop session). */
    fun uncaptured(now: Long, wanted: Boolean, holdable: Boolean): Verdict {
        if (!wanted) {
            since = -1L
            return PASS
        }
        val first = !seen
        seen = true
        if (!passing && since < 0) since = now
        val gaveUp = !passing && now - since >= holdMs
        if (gaveUp) passing = true
        val request = mayRequest(now)
        if (request) requested(now)
        val hold = holdable && !passing
        if (hold) held++
        return Verdict(request, holdMotion = hold, first = first, gaveUp = gaveUp)
    }

    companion object {
        const val RETRY_MS = 250L
        const val HOLD_MS = 1000L
        const val SLOW_RETRY_MS = 2000L
        private val PASS = Verdict(requestCapture = false, holdMotion = false, first = false, gaveUp = false)
    }
}
