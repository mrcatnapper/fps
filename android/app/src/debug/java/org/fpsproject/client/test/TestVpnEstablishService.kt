package org.fpsproject.client.test

import android.content.Intent
import android.net.VpnService
import org.fpsproject.client.VpnServicePlatformHooks
import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunner
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerState
import org.fpsproject.client.nativebridge.HeadlessNativeVpnRuntime
import org.fpsproject.client.nativebridge.LocalCoverClientHandle
import org.fpsproject.client.nativebridge.LocalCoverClientStartResult
import org.fpsproject.client.nativebridge.LocalCoverClientStarter
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.CarrierProbeRuntimePlan
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnRuntimeState
import java.io.IOException
import java.io.InputStream
import java.net.Socket
import kotlin.concurrent.thread

class TestVpnEstablishService : VpnService() {
    private var tun: EstablishedTun? = null
    private var runtime: HeadlessNativeVpnRuntime? = null
    private var productRunner: CoordinatedNativeVpnRunner? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_ESTABLISH -> establishFromIntent(intent)
            ACTION_NATIVE_RUNTIME_START -> startNativeRuntimeFromIntent(intent)
            ACTION_COORDINATED_PRODUCT_FLOW_START -> startCoordinatedProductFlowFromIntent(intent)
            ACTION_NATIVE_RUNTIME_STOP -> {
                closeNativeRuntime(report = true)
                closeProductFlow(report = true)
                stopSelf(startId)
            }
            ACTION_NATIVE_RUNTIME_REVOKE -> {
                onRevoke()
                stopSelf(startId)
            }
            ACTION_CLOSE -> {
                closeTun()
                closeNativeRuntime(report = false)
                closeProductFlow(report = true)
                stopSelf(startId)
            }
        }
        return START_NOT_STICKY
    }

    override fun onRevoke() {
        closeProductFlow(report = true)
        closeNativeRuntime(report = true)
        closeTun()
        TestVpnServiceProbe.reportRevoked()
        super.onRevoke()
    }

    override fun onDestroy() {
        closeProductFlow(report = false)
        closeTun()
        closeNativeRuntime(report = false)
        super.onDestroy()
    }

    private fun establishFromIntent(intent: Intent) {
        closeTun()
        try {
            val profileText = intent.getStringExtra(TestVpnHarnessActivity.EXTRA_PROFILE)
                ?: throw IllegalArgumentException("missing profile")
            val profile = AndroidClientProfileParser.parse(profileText)
            val established = VpnServicePlatformHooks(this).establishTun(profile, leaseFromIntent(intent))
            if (established == null) {
                TestVpnServiceProbe.reportEstablishFailure("establish_returned_null")
                return
            }
            tun = established
            TestVpnServiceProbe.reportEstablished(fd = established.fd, mtu = established.mtu)
        } catch (error: RuntimeException) {
            TestVpnServiceProbe.reportEstablishFailure(error::class.java.simpleName)
        }
    }

    private fun startNativeRuntimeFromIntent(intent: Intent) {
        closeNativeRuntime(report = false)
        closeTun()
        try {
            val profileText = intent.getStringExtra(TestVpnHarnessActivity.EXTRA_PROFILE)
                ?: throw IllegalArgumentException("missing profile")
            val lease = leaseFromIntent(intent)
            val nextRuntime = HeadlessNativeVpnRuntime.create(profileText, VpnServicePlatformHooks(this))
            runtime = nextRuntime
            val started = nextRuntime.start()
            if (started != VpnRuntimeState.WAITING_FOR_LEASE) {
                TestVpnServiceProbe.reportNativeRuntimeFailure("runtime_start_state_$started", nextRuntime.snapshot())
                return
            }
            val running = nextRuntime.onLeaseReceived(lease)
            val snapshot = nextRuntime.snapshot()
            if (running != VpnRuntimeState.RUNNING) {
                TestVpnServiceProbe.reportNativeRuntimeFailure("runtime_lease_state_$running", snapshot)
                return
            }
            TestVpnServiceProbe.reportNativeRuntimeStarted(snapshot)
        } catch (error: RuntimeException) {
            TestVpnServiceProbe.reportNativeRuntimeFailure(error::class.java.simpleName, null)
        }
    }

    private fun startCoordinatedProductFlowFromIntent(intent: Intent) {
        closeProductFlow(report = false)
        closeNativeRuntime(report = false)
        closeTun()
        val profileText = intent.getStringExtra(TestVpnHarnessActivity.EXTRA_PROFILE)
        if (profileText == null) {
            TestVpnServiceProbe.reportProductFlowFailure("missing_profile", null)
            return
        }
        thread(start = true, name = "fps-test-product-flow") {
            val runner = try {
                CoordinatedNativeVpnRunner(
                    runtimeFactory = { HeadlessNativeVpnRuntime.create(profileText, VpnServicePlatformHooks(this)) },
                    coverClientStarter = SyntheticTlsApplicationCoverClientStarter(),
                    tickIntervalMs = 100,
                )
            } catch (error: RuntimeException) {
                TestVpnServiceProbe.reportProductFlowFailure(error::class.java.simpleName, null)
                return@thread
            }
            productRunner = runner
            var snapshot = runner.start()
            var pollsRemaining = PRODUCT_FLOW_POLL_COUNT
            while (pollsRemaining > 0 && !isTerminalProductFlowSnapshot(snapshot)) {
                Thread.sleep(PRODUCT_FLOW_POLL_INTERVAL_MS)
                snapshot = runner.snapshot()
                pollsRemaining -= 1
            }
            if (snapshot.state == CoordinatedNativeVpnRunnerState.RUNNING) {
                TestVpnServiceProbe.reportProductFlowStarted(snapshot)
            } else {
                TestVpnServiceProbe.reportProductFlowFailure(
                    snapshot.lastError ?: "product_flow_state_${snapshot.state}",
                    snapshot,
                )
            }
        }
    }

    private fun closeNativeRuntime(report: Boolean) {
        val activeRuntime = runtime ?: return
        runtime = null
        activeRuntime.close()
        val stoppedSnapshot = activeRuntime.snapshot()
        if (report) {
            TestVpnServiceProbe.reportNativeRuntimeStopped(stoppedSnapshot)
        }
    }

    private fun closeProductFlow(report: Boolean) {
        val activeRunner = productRunner ?: return
        productRunner = null
        val stoppedSnapshot = activeRunner.stop()
        activeRunner.close()
        if (report) {
            TestVpnServiceProbe.reportProductFlowStopped(stoppedSnapshot)
        }
    }

    private fun leaseFromIntent(intent: Intent): TunLease {
        return TunLease(
            clientIpv4 = intent.getLongExtra(TestVpnHarnessActivity.EXTRA_CLIENT_IPV4, 0L),
            serverIpv4 = intent.getLongExtra(TestVpnHarnessActivity.EXTRA_SERVER_IPV4, 0L),
            prefixLength = intent.getIntExtra(TestVpnHarnessActivity.EXTRA_PREFIX_LENGTH, 24),
            mtu = intent.getIntExtra(TestVpnHarnessActivity.EXTRA_MTU, 1280),
        )
    }

    private fun closeTun() {
        val activeTun = tun ?: return
        tun = null
        activeTun.close()
        TestVpnServiceProbe.reportClosed()
    }

    companion object {
        const val ACTION_ESTABLISH = "org.fpsproject.client.test.action.SERVICE_ESTABLISH"
        const val ACTION_CLOSE = "org.fpsproject.client.test.action.SERVICE_CLOSE"
        const val ACTION_NATIVE_RUNTIME_START = "org.fpsproject.client.test.action.NATIVE_RUNTIME_START"
        const val ACTION_NATIVE_RUNTIME_STOP = "org.fpsproject.client.test.action.NATIVE_RUNTIME_STOP"
        const val ACTION_NATIVE_RUNTIME_REVOKE = "org.fpsproject.client.test.action.NATIVE_RUNTIME_REVOKE"
        const val ACTION_COORDINATED_PRODUCT_FLOW_START = "org.fpsproject.client.test.action.COORDINATED_PRODUCT_FLOW_START"

        private const val PRODUCT_FLOW_POLL_COUNT = 100
        private const val PRODUCT_FLOW_POLL_INTERVAL_MS = 100L
    }
}

private fun isTerminalProductFlowSnapshot(snapshot: CoordinatedNativeVpnRunnerSnapshot): Boolean {
    return snapshot.state == CoordinatedNativeVpnRunnerState.RUNNING ||
        snapshot.state == CoordinatedNativeVpnRunnerState.FAILED ||
        snapshot.state == CoordinatedNativeVpnRunnerState.BACKOFF ||
        snapshot.state == CoordinatedNativeVpnRunnerState.NEEDS_VPN_PERMISSION
}

private class SyntheticTlsApplicationCoverClientStarter : LocalCoverClientStarter {
    override fun start(localBridgePort: Int, carrierPlan: CarrierProbeRuntimePlan): LocalCoverClientStartResult {
        var socket: Socket? = null
        return try {
            val connected = Socket("127.0.0.1", localBridgePort)
            socket = connected
            connected.soTimeout = 5000
            val output = connected.getOutputStream()
            output.write(tlsApplicationDataRecord("android-product-cover-1".encodeToByteArray()))
            output.flush()
            readTlsRecord(connected.getInputStream())
            output.write(tlsApplicationDataRecord("android-product-cover-2".encodeToByteArray()))
            output.flush()
            socket = null
            LocalCoverClientStartResult.started(SocketCoverClientHandle(connected))
        } catch (_: IOException) {
            socket?.close()
            LocalCoverClientStartResult.failed("cover_io_failed")
        } catch (_: RuntimeException) {
            socket?.close()
            LocalCoverClientStartResult.failed("cover_io_failed")
        }
    }
}

private class SocketCoverClientHandle(
    private val socket: Socket,
) : LocalCoverClientHandle {
    override fun close() {
        socket.close()
    }
}

private fun tlsApplicationDataRecord(payload: ByteArray): ByteArray {
    require(payload.size <= 0xffff)
    return byteArrayOf(
        0x17,
        0x03,
        0x03,
        ((payload.size ushr 8) and 0xff).toByte(),
        (payload.size and 0xff).toByte(),
    ) + payload
}

private fun readTlsRecord(input: InputStream): ByteArray {
    val header = readExact(input, 5)
    val payloadLength = ((header[3].toInt() and 0xff) shl 8) or (header[4].toInt() and 0xff)
    return header + readExact(input, payloadLength)
}

private fun readExact(input: InputStream, size: Int): ByteArray {
    val out = ByteArray(size)
    var offset = 0
    while (offset < size) {
        val read = input.read(out, offset, size - offset)
        if (read < 0) {
            throw IOException("unexpected EOF")
        }
        offset += read
    }
    return out
}
