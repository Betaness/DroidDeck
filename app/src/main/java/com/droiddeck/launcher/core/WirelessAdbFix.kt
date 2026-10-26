package com.droiddeck.launcher.core

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import com.droiddeck.launcher.R
import com.flyfishxu.kadb.Kadb
import com.flyfishxu.kadb.cert.KadbCert
import java.io.File
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.ArrayDeque

object WirelessAdbFix {
    private const val TAG = "WirelessAdbFix"
    private const val CERT_FILE = "wireless-adb-cert.pem"
    private const val KEY_FILE = "wireless-adb-key.pem"
    private const val HOST_FILE = "wireless-adb-host.txt"
    private const val CONNECTION_FILE = "wireless-adb-connection.txt"
    const val LOOPBACK = "127.0.0.1"
    /** Ports from a loopback scan given a connection, at most; adbd has one. */
    private const val SCANNED_ATTEMPTS = 3
    /** Callers that come together at a session's start look for a new port one at a time. */
    private val discovery = Any()

    private data class SavedConnection(val host: String, val port: Int?)

    @Synchronized
    private fun loadOrCreateIdentity(context: Context) {
        val directory = context.noBackupFilesDir
        val cert = File(directory, CERT_FILE)
        val key = File(directory, KEY_FILE)
        if (cert.isFile && key.isFile) {
            KadbCert.set(cert.readBytes(), key.readBytes())
            return
        }

        val identity = KadbCert.get(notAfter = System.currentTimeMillis() + TimeUnit.DAYS.toMillis(3650))
        KadbCert.set(identity.first, identity.second)
        writeAtomically(context, cert, identity.first)
        writeAtomically(context, key, identity.second)
    }

    private fun writeAtomically(context: Context, destination: File, contents: ByteArray) {
        val temporary = File(destination.parentFile, "${destination.name}.tmp")
        temporary.writeBytes(contents)
        check(temporary.renameTo(destination)) { context.getString(R.string.adbfix_save_failed) }
    }

    suspend fun pair(context: Context, host: String, port: Int, pairingCode: String) {
        loadOrCreateIdentity(context.applicationContext)
        Kadb.pair(host, port, pairingCode, "DroidDeck")
        writeAtomically(context, File(context.noBackupFilesDir, HOST_FILE), host.toByteArray())
        writeAtomically(context, File(context.noBackupFilesDir, CONNECTION_FILE), ByteArray(0))
    }

    private fun savedConnection(context: Context): SavedConnection? {
        val directory = context.applicationContext.noBackupFilesDir
        if (!File(directory, CERT_FILE).isFile || !File(directory, KEY_FILE).isFile) return null
        val host = File(directory, HOST_FILE).takeIf(File::isFile)?.readText()?.trim()
            ?.takeIf(String::isNotEmpty) ?: return null
        val lines = File(directory, CONNECTION_FILE).takeIf(File::isFile)?.readLines()
        val port = lines?.takeIf { it.size == 2 && it[0] == host }
            ?.get(1)?.toIntOrNull()?.takeIf { it in 1..65535 }
        return SavedConnection(host, port)
    }

    fun savedConnectionAddress(context: Context): String? = savedConnection(context)?.let { saved ->
        val host = if (':' in saved.host) "[${saved.host}]" else saved.host
        "$host:${saved.port ?: ""}"
    }

    private fun saveConnection(context: Context, host: String, port: Int) {
        writeAtomically(
            context,
            File(context.applicationContext.noBackupFilesDir, CONNECTION_FILE),
            "$host\n$port".toByteArray(),
        )
    }

    fun isLocalAddress(address: InetAddress): Boolean = runCatching {
        if (address.isLoopbackAddress) return true
        NetworkInterface.getNetworkInterfaces()?.toList().orEmpty().any { network ->
            network.inetAddresses.toList().any { it.address.contentEquals(address.address) }
        }
    }.getOrDefault(address is Inet4Address)

    fun localConnectPort(context: Context): Int? =
        tlsPortProperty()
            ?: findConnectPort(context, LOOPBACK)

    private fun tlsPortProperty(): Int? = runCatching {
        Class.forName("android.os.SystemProperties").getMethod("get", String::class.java)
            .invoke(null, "service.adb.tls.port") as? String
    }.getOrNull()?.trim()?.toIntOrNull()?.takeIf { it in 1..65535 }

    fun findConnectPort(context: Context, pairedHost: String, timeoutSeconds: Long = 12): Int? {
        val manager = context.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
        val paired = runCatching { InetAddress.getByName(pairedHost) }.getOrNull() ?: return null
        val matches: (InetAddress) -> Boolean =
            if (paired.isLoopbackAddress) ::isLocalAddress else { address -> address.address.contentEquals(paired.address) }
        val resolvedPort = AtomicInteger(-1)
        val finished = AtomicBoolean(false)
        val latch = CountDownLatch(1)
        val queue = ArrayDeque<NsdServiceInfo>()
        val seen = mutableSetOf<String>()
        var resolving = false
        val lock = Any()

        lateinit var resolveNext: () -> Unit
        resolveNext = {
            val next = synchronized(lock) {
                if (finished.get() || resolving || queue.isEmpty()) null
                else queue.removeFirst().also { resolving = true }
            }
            if (next != null) {
                val listener = object : NsdManager.ResolveListener {
                    override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                        synchronized(lock) { resolving = false }
                        resolveNext()
                    }

                    override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                        val address = serviceInfo.host
                        if (address != null && matches(address) && finished.compareAndSet(false, true)) {
                            resolvedPort.set(serviceInfo.port)
                            latch.countDown()
                        } else {
                            synchronized(lock) { resolving = false }
                            resolveNext()
                        }
                    }
                }
                runCatching { manager.resolveService(next, listener) }.onFailure {
                    synchronized(lock) { resolving = false }
                    resolveNext()
                }
            }
        }

        val discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) = Unit
            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                if (serviceInfo.serviceType?.contains("_adb-tls-connect._tcp") == true) {
                    synchronized(lock) {
                        if (seen.add(serviceInfo.serviceName)) queue.addLast(serviceInfo)
                    }
                    resolveNext()
                }
            }
            override fun onServiceLost(serviceInfo: NsdServiceInfo) = Unit
            override fun onDiscoveryStopped(serviceType: String) { latch.countDown() }
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { latch.countDown() }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) { latch.countDown() }
        }

        try {
            manager.discoverServices("_adb-tls-connect._tcp.", NsdManager.PROTOCOL_DNS_SD, discoveryListener)
            latch.await(timeoutSeconds, TimeUnit.SECONDS)
        } catch (_: Exception) {
            return null
        } finally {
            runCatching { manager.stopServiceDiscovery(discoveryListener) }
        }
        return resolvedPort.get().takeIf { it in 1..65535 }
    }

    /** A shell over the pairing just made, saved for later connections; the child process limit stays as it is. */
    fun verifyPairing(context: Context, host: String, port: Int) {
        loadOrCreateIdentity(context.applicationContext)
        Kadb.create(host, port).use { adb ->
            val result = adb.shell("echo ok")
            check(result.output.trim() == "ok") { result.allOutput.ifBlank { context.getString(R.string.adbfix_command_failed, result.exitCode) } }
        }
        saveConnection(context, host, port)
    }

    fun setChildProcessLimit(context: Context, host: String, port: Int, enabled: Boolean) {
        loadOrCreateIdentity(context.applicationContext)
        Kadb.create(host, port).use { adb ->
            PhantomProcessLimit.shellCommands(enabled).forEach { command ->
                val change = adb.shell(command)
                check(change.exitCode == 0) { change.allOutput.ifBlank { context.getString(R.string.adbfix_command_failed, change.exitCode) } }
            }
            val result = adb.shell(PhantomProcessLimit.verifyCommand())
            check(result.exitCode == 0 && PhantomProcessLimit.verified(result.output, enabled)) {
                context.getString(R.string.adbfix_not_confirmed, result.allOutput.trim())
            }
        }
        if (PhantomProcessLimit.usesDeviceConfig()) PhantomProcessLimit.rememberAndroid12(context, !enabled)
        saveConnection(context, host, port)
    }

    /** A shell-user connection that answers, or null. */
    private fun attempt(host: String, port: Int): Kadb? {
        val adb = runCatching { Kadb.create(host, port, connectTimeout = 3000) }.getOrNull() ?: return null
        if (runCatching { adb.shell("echo ok").output.trim() == "ok" }.getOrDefault(false)) return adb
        runCatching { adb.close() }
        return null
    }

    /**
     * [mali-g2] The wireless debugging port of this device by a loopback scan ([AdbPortScan]) and a connection to it,
     * skipping the ports [tried] already. Saved as a loopback connection: the pairing holds for this device's adbd
     * whatever address it was made on, and that address may have changed since.
     */
    private fun scanLoopback(appContext: Context, tried: Set<Int>): Pair<Int, Kadb>? {
        val range = AdbPortScan.range()
        val startedAt = System.nanoTime()
        val ports = try {
            AdbPortScan.scan(LOOPBACK, AdbPortScan.order(range, tried))
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
            return null
        }
        Log.i(TAG, "loopback ports ${range.first}-${range.last} answering as wireless debugging: " +
            "${ports.ifEmpty { "none" }} (${TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - startedAt)} ms)")
        for (port in ports.take(SCANNED_ATTEMPTS)) {
            if (Thread.currentThread().isInterrupted) return null
            val adb = attempt(LOOPBACK, port) ?: continue
            writeAtomically(appContext, File(appContext.noBackupFilesDir, HOST_FILE), LOOPBACK.toByteArray())
            saveConnection(appContext, LOOPBACK, port)
            return port to adb
        }
        if (ports.isNotEmpty()) Log.i(TAG, "no answer through the saved pairing on $ports: paired again since, or the key revoked?")
        return null
    }

    /**
     * A shell-user connection to this device through the saved pairing (the cached port, else the one
     * announced over mDNS, else one found by a loopback scan), or null without a pairing or when wireless
     * debugging is off. The caller closes it. Blocks for up to about 20 s when the cached port is stale.
     */
    fun connectUsingSavedPairing(context: Context): Kadb? {
        val appContext = context.applicationContext
        val first = savedConnection(appContext) ?: return null
        loadOrCreateIdentity(appContext)
        first.port?.let { port -> attempt(first.host, port)?.let { return it } }
        synchronized(discovery) {
            val tried = mutableSetOf<Int>()
            first.port?.let { tried += it }
            // Another caller may have found the new port meanwhile.
            val saved = savedConnection(appContext) ?: return null
            saved.port?.takeIf { saved != first }?.let { port ->
                tried += port
                attempt(saved.host, port)?.let { return it }
            }
            findConnectPort(appContext, saved.host)?.takeIf { tried.add(it) }?.let { port ->
                attempt(saved.host, port)?.let { adb ->
                    saveConnection(appContext, saved.host, port)
                    return adb
                }
            }
            return scanLoopback(appContext, tried)?.second
        }
    }

    fun setUsingSavedPairing(context: Context, enabled: Boolean) {
        val appContext = context.applicationContext
        val saved = savedConnection(appContext)
            ?: error(appContext.getString(R.string.adbfix_no_pairing))
        val cachedPort = saved.port
        val cachedResult = cachedPort?.let { port ->
            runCatching { setChildProcessLimit(appContext, saved.host, port, enabled) }.also { if (it.isSuccess) return }
        }
        synchronized(discovery) {
            val discoveredPort = findConnectPort(appContext, saved.host)
            if (discoveredPort != null && discoveredPort != cachedPort) {
                setChildProcessLimit(appContext, saved.host, discoveredPort, enabled)
                return
            }
            loadOrCreateIdentity(appContext)
            val scanned = scanLoopback(appContext, setOfNotNull(cachedPort))
            if (scanned != null) {
                scanned.second.close()
                setChildProcessLimit(appContext, LOOPBACK, scanned.first, enabled)
                return
            }
        }
        cachedResult?.getOrThrow()
        error(appContext.getString(R.string.adbfix_enter_address))
    }
}
