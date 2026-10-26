package com.droiddeck.launcher.core

import java.io.DataInputStream
import java.io.File
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * [mali-g2] This device's wireless debugging port, found by trying loopback ports, for when mDNS does not
 * announce it (HyperOS on the Xring O3 stopped resolving _adb-tls-connect for the app while the port was open).
 *
 * adbd listens on an ephemeral port. A port that answers an adb CNXN with STLS, the request to start TLS, is a
 * wireless debugging port; nothing is sent past that, so no key is offered. Closed loopback ports refuse at
 * once, which makes the whole ephemeral range a fraction of a second over a few threads.
 */
internal object AdbPortScan {
    private const val PORT_RANGE_FILE = "/proc/sys/net/ipv4/ip_local_port_range"
    /** Linux's default ephemeral range, for when the kernel's cannot be read. */
    val DEFAULT_RANGE = 32768..60999

    private const val A_CNXN = 0x4e584e43
    private const val A_STLS = 0x534c5453
    private const val A_VERSION = 0x01000001
    private const val MAX_PAYLOAD = 1024 * 1024
    private const val HEADER_SIZE = 24

    /** "low\thigh" as in ip_local_port_range; null when malformed. */
    fun parseRange(text: String?): IntRange? {
        val parts = text?.trim()?.split(Regex("\\s+"))?.takeIf { it.size == 2 } ?: return null
        val low = parts[0].toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
        val high = parts[1].toIntOrNull()?.takeIf { it in low..65535 } ?: return null
        return low..high
    }

    fun range(): IntRange = parseRange(runCatching { File(PORT_RANGE_FILE).readText() }.getOrNull()) ?: DEFAULT_RANGE

    /** The ports to try: [range] in ascending order, without those tried already. */
    fun order(range: IntRange, tried: Set<Int>): IntArray = range.filter { it !in tried }.toIntArray()

    /** An adb CNXN as a host sends it first. */
    fun connectMessage(): ByteArray {
        val payload = "host::\u0000".toByteArray(Charsets.US_ASCII)
        return ByteBuffer.allocate(HEADER_SIZE + payload.size).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(A_CNXN).putInt(A_VERSION).putInt(MAX_PAYLOAD).putInt(payload.size)
            .putInt(payload.sumOf { it.toInt() and 0xff }).putInt(A_CNXN.inv())
            .put(payload).array()
    }

    /** Whether [header] is an adb STLS message. */
    fun isStls(header: ByteArray): Boolean {
        if (header.size < HEADER_SIZE) return false
        val b = ByteBuffer.wrap(header, 0, HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        val command = b.getInt(0)
        return command == A_STLS && b.getInt(20) == command.inv()
    }

    private fun answersStls(address: InetAddress, port: Int, connectTimeoutMs: Int, replyTimeoutMs: Int): Boolean =
        runCatching {
            Socket().use { socket ->
                socket.connect(InetSocketAddress(address, port), connectTimeoutMs)
                socket.soTimeout = replyTimeoutMs
                socket.tcpNoDelay = true
                socket.getOutputStream().apply { write(connectMessage()); flush() }
                val header = ByteArray(HEADER_SIZE)
                DataInputStream(socket.getInputStream()).readFully(header)
                isStls(header)
            }
        }.getOrDefault(false)

    /**
     * Those of [ports] on [host] that answer as a wireless debugging port, ascending. Bounded by [deadlineMs]:
     * ports not reached by then are left out.
     */
    fun scan(
        host: String,
        ports: IntArray,
        deadlineMs: Long = 4_000,
        threads: Int = 16,
        connectTimeoutMs: Int = 250,
        replyTimeoutMs: Int = 400,
    ): List<Int> {
        val address = InetAddress.getByName(host)
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(deadlineMs)
        val next = AtomicInteger()
        val done = AtomicBoolean(false)
        val found = ConcurrentLinkedQueue<Int>()
        val workers = List(threads.coerceIn(1, 64)) {
            Thread({
                while (!done.get() && System.nanoTime() < deadline) {
                    val i = next.getAndIncrement()
                    if (i >= ports.size) break
                    if (answersStls(address, ports[i], connectTimeoutMs, replyTimeoutMs)) found.add(ports[i])
                }
            }, "adb-port-scan").apply { isDaemon = true; start() }
        }
        try {
            for (worker in workers) {
                val left = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime())
                // A worker past the deadline finishes its port within the socket timeouts.
                worker.join((left + connectTimeoutMs + replyTimeoutMs).coerceAtLeast(1))
            }
        } finally {
            done.set(true)
        }
        return found.toList().sorted()
    }
}
