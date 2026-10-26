package com.droiddeck.launcher.core

import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AdbPortScanTest {
    @Test fun `the kernel's port range is read`() {
        assertEquals(32768..60999, AdbPortScan.parseRange("32768\t60999\n"))
        assertEquals(30000..48000, AdbPortScan.parseRange(" 30000   48000 "))
        assertNull(AdbPortScan.parseRange(null))
        assertNull(AdbPortScan.parseRange(""))
        assertNull(AdbPortScan.parseRange("32768"))
        assertNull(AdbPortScan.parseRange("60999 32768"))
        assertNull(AdbPortScan.parseRange("0 100"))
        assertNull(AdbPortScan.parseRange("1 70000"))
        assertNull(AdbPortScan.parseRange("a b"))
    }

    @Test fun `ports are tried in ascending order without those tried already`() {
        assertArrayEquals(intArrayOf(40000, 40002, 40004), AdbPortScan.order(40000..40004, setOf(40001, 40003, 50000)))
        assertArrayEquals(intArrayOf(), AdbPortScan.order(40000..40000, setOf(40000)))
    }

    @Test fun `the CNXN message is a valid adb header`() {
        val message = AdbPortScan.connectMessage()
        val b = ByteBuffer.wrap(message).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0x4e584e43, b.getInt(0))
        assertEquals(message.size - 24, b.getInt(12))
        assertEquals(b.getInt(0).inv(), b.getInt(20))
        assertEquals("host::\u0000", String(message, 24, message.size - 24, Charsets.US_ASCII))
    }

    @Test fun `only STLS with its magic counts`() {
        assertTrue(AdbPortScan.isStls(header(0x534c5453)))
        assertFalse(AdbPortScan.isStls(header(0x534c5453, magic = 0)))
        assertFalse(AdbPortScan.isStls(header(0x48545541))) // AUTH: an adb over plain TCP, not wireless debugging
        assertFalse(AdbPortScan.isStls(ByteArray(10)))
    }

    @Test fun `a scan finds the port that answers STLS and nothing else`() {
        val loopback = InetAddress.getByName("127.0.0.1")
        val adbd = ServerSocket(0, 50, loopback)
        val other = ServerSocket(0, 50, loopback) // answers with something else
        val silent = ServerSocket(0, 50, loopback) // never answers
        val closed = ServerSocket(0, 50, loopback).let { val port = it.localPort; it.close(); port }
        serve(adbd) { header(0x534c5453) }
        serve(other) { "HTTP/1.1 400 Bad Request\r\n\r\n".padEnd(24).toByteArray() }
        val ports = intArrayOf(closed, silent.localPort, other.localPort, adbd.localPort)
        try {
            val started = System.nanoTime()
            assertEquals(listOf(adbd.localPort), AdbPortScan.scan("127.0.0.1", ports, deadlineMs = 2_000, threads = 4, replyTimeoutMs = 300))
            assertTrue("bounded by the reply timeout", System.nanoTime() - started < 2_000_000_000L)
        } finally {
            listOf(adbd, other, silent).forEach { it.close() }
        }
    }

    @Test fun `a scan stops at its deadline`() {
        val started = System.nanoTime()
        val silent = List(4) { ServerSocket(0, 50, InetAddress.getByName("127.0.0.1")) }
        try {
            val ports = IntArray(40) { silent[it % silent.size].localPort }
            assertEquals(emptyList<Int>(), AdbPortScan.scan("127.0.0.1", ports, deadlineMs = 300, threads = 2, replyTimeoutMs = 200))
            // 40 ports at 200 ms over 2 threads would take 4 s.
            assertTrue(System.nanoTime() - started < 2_000_000_000L)
        } finally {
            silent.forEach { it.close() }
        }
    }

    private fun header(command: Int, magic: Int = command.inv()): ByteArray =
        ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(command).putInt(0x01000000).putInt(0).putInt(0).putInt(0).putInt(magic).array()

    private fun serve(server: ServerSocket, reply: () -> ByteArray) {
        Thread {
            while (!server.isClosed) {
                val socket = runCatching { server.accept() }.getOrNull() ?: break
                runCatching {
                    socket.use {
                        DataInputStream(it.getInputStream()).readFully(ByteArray(24))
                        it.getOutputStream().write(reply())
                    }
                }
            }
        }.apply { isDaemon = true; start() }
    }
}
