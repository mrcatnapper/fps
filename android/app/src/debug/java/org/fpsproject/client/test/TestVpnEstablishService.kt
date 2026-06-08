package org.fpsproject.client.test

import android.content.Intent
import android.net.VpnService
import org.fpsproject.client.VpnServicePlatformHooks
import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.nativebridge.HeadlessNativeVpnRuntime
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnRuntimeState

class TestVpnEstablishService : VpnService() {
    private var tun: EstablishedTun? = null
    private var runtime: HeadlessNativeVpnRuntime? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_ESTABLISH -> establishFromIntent(intent)
            ACTION_NATIVE_RUNTIME_START -> startNativeRuntimeFromIntent(intent)
            ACTION_NATIVE_RUNTIME_STOP -> {
                closeNativeRuntime(report = true)
                stopSelf(startId)
            }
            ACTION_NATIVE_RUNTIME_REVOKE -> {
                onRevoke()
                stopSelf(startId)
            }
            ACTION_CLOSE -> {
                closeTun()
                closeNativeRuntime(report = false)
                stopSelf(startId)
            }
        }
        return START_NOT_STICKY
    }

    override fun onRevoke() {
        closeNativeRuntime(report = true)
        closeTun()
        TestVpnServiceProbe.reportRevoked()
        super.onRevoke()
    }

    override fun onDestroy() {
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

    private fun closeNativeRuntime(report: Boolean) {
        val activeRuntime = runtime ?: return
        runtime = null
        activeRuntime.close()
        val stoppedSnapshot = activeRuntime.snapshot()
        if (report) {
            TestVpnServiceProbe.reportNativeRuntimeStopped(stoppedSnapshot)
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
    }
}
