package org.fpsproject.client.test

import android.content.Intent
import android.net.VpnService
import org.fpsproject.client.AndroidVpnTunnelBuilder
import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnTunEstablisher

class TestVpnEstablishService : VpnService() {
    private var tun: EstablishedTun? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_ESTABLISH -> establishFromIntent(intent)
            ACTION_CLOSE -> {
                closeTun()
                stopSelf(startId)
            }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        closeTun()
        super.onDestroy()
    }

    private fun establishFromIntent(intent: Intent) {
        closeTun()
        try {
            val profileText = intent.getStringExtra(TestVpnHarnessActivity.EXTRA_PROFILE)
                ?: throw IllegalArgumentException("missing profile")
            val profile = AndroidClientProfileParser.parse(profileText)
            val lease = TunLease(
                clientIpv4 = intent.getLongExtra(TestVpnHarnessActivity.EXTRA_CLIENT_IPV4, 0L),
                serverIpv4 = intent.getLongExtra(TestVpnHarnessActivity.EXTRA_SERVER_IPV4, 0L),
                prefixLength = intent.getIntExtra(TestVpnHarnessActivity.EXTRA_PREFIX_LENGTH, 24),
                mtu = intent.getIntExtra(TestVpnHarnessActivity.EXTRA_MTU, 1280),
            )
            val established = VpnTunEstablisher { AndroidVpnTunnelBuilder(this) }.establish(profile, lease)
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

    private fun closeTun() {
        val activeTun = tun ?: return
        tun = null
        activeTun.close()
        TestVpnServiceProbe.reportClosed()
    }

    companion object {
        const val ACTION_ESTABLISH = "org.fpsproject.client.test.action.SERVICE_ESTABLISH"
        const val ACTION_CLOSE = "org.fpsproject.client.test.action.SERVICE_CLOSE"
    }
}
