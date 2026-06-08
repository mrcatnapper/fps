package org.fpsproject.client.test

import android.app.Activity
import android.content.Intent
import android.net.VpnService
import android.os.Bundle

class TestVpnHarnessActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prepareIntent = VpnService.prepare(this)
        if (prepareIntent == null) {
            onPrepared()
            return
        }
        startActivityForResult(prepareIntent, REQUEST_VPN_PERMISSION)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_VPN_PERMISSION) {
            finish()
            return
        }
        if (resultCode == RESULT_OK) {
            onPrepared()
        } else {
            TestVpnServiceProbe.reportPermission(granted = false)
            finish()
        }
    }

    private fun onPrepared() {
        TestVpnServiceProbe.reportPermission(granted = true)
        if (intent.action == ACTION_ESTABLISH) {
            val serviceIntent = Intent(this, TestVpnEstablishService::class.java).apply {
                action = TestVpnEstablishService.ACTION_ESTABLISH
                putExtras(intent)
            }
            startService(serviceIntent)
        }
        finish()
    }

    companion object {
        const val ACTION_ESTABLISH = "org.fpsproject.client.test.action.ESTABLISH"
        const val EXTRA_PROFILE = "org.fpsproject.client.test.extra.PROFILE"
        const val EXTRA_CLIENT_IPV4 = "org.fpsproject.client.test.extra.CLIENT_IPV4"
        const val EXTRA_SERVER_IPV4 = "org.fpsproject.client.test.extra.SERVER_IPV4"
        const val EXTRA_PREFIX_LENGTH = "org.fpsproject.client.test.extra.PREFIX_LENGTH"
        const val EXTRA_MTU = "org.fpsproject.client.test.extra.MTU"

        private const val REQUEST_VPN_PERMISSION = 1001
    }
}
