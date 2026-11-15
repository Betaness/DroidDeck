package com.droiddeck.launcher.input

/**
 * [mali-g2] Where the compositor's pointer is on the output view, counting the relative motion it has
 * not read yet.
 *
 * Pointer deltas reach the compositor through a pipe it reads on its own thread. It publishes its pointer
 * in scene px with the number of the last delta it read (WaylandCompositor.nativeCursorScene), and the
 * deltas sent after that one are added here, clamped to the scene and to a confined pointer's box as the
 * compositor clamps them. A session below the output resolution shows one scene px as more than one view px.
 */
class GuestPointerTrack @JvmOverloads constructor(
    private val capacity: Int = 256,
    private val maxAgeMs: Long = 500,
) {
    private val seqs = IntArray(capacity)
    private val dxs = IntArray(capacity)
    private val dys = IntArray(capacity)
    private val times = LongArray(capacity)
    private var first = 0
    private var count = 0
    internal var lastSeq = 0

    /** Records a delta in 1/256 scene px that is about to be sent and returns its number, never 0. */
    @Synchronized
    fun add(dx256: Int, dy256: Int, nowMs: Long): Int {
        lastSeq++
        if (lastSeq == 0) lastSeq = 1
        if (count == capacity) {
            first = (first + 1) % capacity
            count--
        }
        val i = (first + count) % capacity
        seqs[i] = lastSeq
        dxs[i] = dx256
        dys[i] = dy256
        times[i] = nowMs
        count++
        return lastSeq
    }

    /**
     * Writes the pointer's position on a [viewWidth] x [viewHeight] view into [out] from the compositor's
     * [state] (layout: the constants below). Deltas that the compositor has read, or that were sent more
     * than maxAgeMs ago, are forgotten. False without a scene or a view.
     */
    @Synchronized
    fun place(state: DoubleArray, nowMs: Long, viewWidth: Int, viewHeight: Int, out: FloatArray): Boolean {
        val w = state[SCENE_W].toInt()
        val h = state[SCENE_H].toInt()
        if (state.size < STATE_SIZE || w <= 0 || h <= 0 || viewWidth <= 0 || viewHeight <= 0) return false
        val applied = state[SEQ].toLong().toInt()
        while (count > 0 && (read(seqs[first], applied) || nowMs - times[first] > maxAgeMs)) {
            first = (first + 1) % capacity
            count--
        }
        val boxX = state[BOX_X0] <= state[BOX_X1]
        val boxY = state[BOX_Y0] <= state[BOX_Y1]
        var x = state[X]
        var y = state[Y]
        for (n in 0 until count) {
            val i = (first + n) % capacity
            x = (x + dxs[i] / 256.0).coerceIn(0.0, w - 1.0)
            y = (y + dys[i] / 256.0).coerceIn(0.0, h - 1.0)
            if (boxX) x = x.coerceIn(state[BOX_X0], state[BOX_X1])
            if (boxY) y = y.coerceIn(state[BOX_Y0], state[BOX_Y1])
        }
        out[0] = ((state[MAP_AX] * x + state[MAP_BX]) * viewWidth).toFloat()
        out[1] = ((state[MAP_AY] * y + state[MAP_BY]) * viewHeight).toFloat()
        return true
    }

    /** Deltas not read by the compositor yet, as of the last [place]. */
    @Synchronized
    fun pending(): Int = count

    private fun read(seq: Int, applied: Int): Boolean = applied != 0 && applied - seq >= 0

    companion object {
        /** The pointer in scene px. */
        const val X = 0
        const val Y = 1
        const val SCENE_W = 2
        const val SCENE_H = 3
        /** A scene position as fractions of the output: x * MAP_AX + MAP_BX, y * MAP_AY + MAP_BY. */
        const val MAP_AX = 4
        const val MAP_BX = 5
        const val MAP_AY = 6
        const val MAP_BY = 7
        /** The number of the last delta the compositor read, 0 before the first. */
        const val SEQ = 8
        /** The box the pointer stays in, scene px, inclusive: the scene, or a confined pointer's box. */
        const val BOX_X0 = 9
        const val BOX_Y0 = 10
        const val BOX_X1 = 11
        const val BOX_Y1 = 12
        const val STATE_SIZE = 13
    }
}
