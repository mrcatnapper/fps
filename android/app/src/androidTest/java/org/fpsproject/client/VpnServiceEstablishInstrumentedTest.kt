package org.fpsproject.client

import android.content.Context
import android.content.Intent
import android.net.VpnService
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.Until
import org.fpsproject.client.test.TestVpnEstablishService
import org.fpsproject.client.test.TestVpnHarnessActivity
import org.fpsproject.client.test.TestVpnServiceProbe
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.fpsproject.client.nativebridge.TUN_FD_OWNERSHIP_OWNED_DUPLICATE
import org.fpsproject.client.runtime.VpnRuntimeState
import java.util.Base64
import java.util.regex.Pattern

class VpnServiceEstablishInstrumentedTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val targetContext: Context = instrumentation.targetContext

    @Test
    fun realVpnServiceBuilderEstablishesTunFdAndClosesIt() {
        TestVpnServiceProbe.reset()

        targetContext.startActivity(establishIntent())
        acceptVpnPermissionDialogIfPresent()

        val permission = TestVpnServiceProbe.awaitPermission(timeoutMs = 15_000)
        assertNotNull("VPN permission result was not reported", permission)
        assertTrue("VPN permission was denied", permission!!.granted)
        assertEquals("VPN permission should be prepared after consent", null, VpnService.prepare(targetContext))

        val established = TestVpnServiceProbe.awaitEstablished(timeoutMs = 30_000)
        assertNotNull("TUN establish result was not reported", established)
        assertTrue("TUN establish failed: ${established!!.error}", established.success)
        assertTrue("TUN fd must be non-negative", established.fd >= 0)
        assertEquals(1280, established.mtu)

        targetContext.startService(Intent(targetContext, TestVpnEstablishService::class.java).apply {
            action = TestVpnEstablishService.ACTION_CLOSE
        })
        assertTrue("TUN close was not reported", TestVpnServiceProbe.awaitClosed(timeoutMs = 10_000))
    }

    @Test
    fun realVpnServiceFdFlowsThroughHeadlessNativeRuntimeAndStopsCleanly() {
        TestVpnServiceProbe.reset()

        targetContext.startActivity(nativeRuntimeIntent())
        acceptVpnPermissionDialogIfPresent()

        val permission = TestVpnServiceProbe.awaitPermission(timeoutMs = 15_000)
        assertNotNull("VPN permission result was not reported", permission)
        assertTrue("VPN permission was denied", permission!!.granted)

        val started = TestVpnServiceProbe.awaitNativeRuntimeStarted(timeoutMs = 30_000)
        assertNotNull("Native runtime start result was not reported", started)
        assertTrue("Native runtime failed: ${started!!.error}", started.success)
        val startedSnapshot = started.snapshot!!
        assertEquals(VpnRuntimeState.RUNNING, startedSnapshot.vpn.state)
        assertTrue(startedSnapshot.vpn.tun.fdPresent)
        assertTrue(startedSnapshot.native.alive)
        assertTrue(startedSnapshot.native.started)
        assertTrue(startedSnapshot.native.workerThreadRunning)
        assertTrue(startedSnapshot.native.tunAttached)
        assertEquals(TUN_FD_OWNERSHIP_OWNED_DUPLICATE, startedSnapshot.native.tunFdOwnership)
        assertTrue(startedSnapshot.native.tunPumpRunning)
        assertEquals(1280, startedSnapshot.native.tunMtu)

        targetContext.startService(Intent(targetContext, TestVpnEstablishService::class.java).apply {
            action = TestVpnEstablishService.ACTION_NATIVE_RUNTIME_STOP
        })
        val stopped = TestVpnServiceProbe.awaitNativeRuntimeStopped(timeoutMs = 10_000)
        assertNotNull("Native runtime stop result was not reported", stopped)
        assertEquals(VpnRuntimeState.STOPPED, stopped!!.vpn.state)
        assertFalse(stopped.native.started)
        assertFalse(stopped.native.workerThreadRunning)
        assertFalse(stopped.native.tunPumpRunning)
        assertFalse(stopped.vpn.tun.fdPresent)
    }

    @Test
    fun nativeRuntimeVpnServiceRevokeStopsRuntimeCleanly() {
        TestVpnServiceProbe.reset()

        targetContext.startActivity(nativeRuntimeIntent())
        acceptVpnPermissionDialogIfPresent()

        val started = TestVpnServiceProbe.awaitNativeRuntimeStarted(timeoutMs = 30_000)
        assertNotNull("Native runtime start result was not reported", started)
        assertTrue("Native runtime failed: ${started!!.error}", started.success)

        targetContext.startService(Intent(targetContext, TestVpnEstablishService::class.java).apply {
            action = TestVpnEstablishService.ACTION_NATIVE_RUNTIME_REVOKE
        })
        val stopped = TestVpnServiceProbe.awaitNativeRuntimeStopped(timeoutMs = 10_000)
        assertTrue("Revoke was not reported", TestVpnServiceProbe.awaitRevoked(timeoutMs = 10_000))
        assertNotNull("Native runtime stop result was not reported", stopped)
        assertEquals(VpnRuntimeState.STOPPED, stopped!!.vpn.state)
        assertFalse(stopped.native.tunPumpRunning)
        assertFalse(stopped.vpn.tun.fdPresent)
    }

    private fun establishIntent(): Intent {
        return Intent(targetContext, TestVpnHarnessActivity::class.java).apply {
            action = TestVpnHarnessActivity.ACTION_ESTABLISH
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            putExtra(TestVpnHarnessActivity.EXTRA_PROFILE, profileJson())
            putExtra(TestVpnHarnessActivity.EXTRA_CLIENT_IPV4, 0x0a420002L)
            putExtra(TestVpnHarnessActivity.EXTRA_SERVER_IPV4, 0x0a420001L)
            putExtra(TestVpnHarnessActivity.EXTRA_PREFIX_LENGTH, 24)
            putExtra(TestVpnHarnessActivity.EXTRA_MTU, 1280)
        }
    }

    private fun nativeRuntimeIntent(): Intent {
        return establishIntent().apply {
            action = TestVpnHarnessActivity.ACTION_NATIVE_RUNTIME
        }
    }

    private fun acceptVpnPermissionDialogIfPresent() {
        val device = UiDevice.getInstance(instrumentation)
        device.waitForIdle()
        val button = device.wait(Until.findObject(By.res("android:id/button1")), 5_000)
            ?: device.wait(Until.findObject(By.text(Pattern.compile("(?i)(OK|Allow|Connect|Turn on|Yes)"))), 2_000)
        button?.click()
        device.waitForIdle()
    }

    private fun profileJson(): String {
        val key = Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })
        return """
            {
              "network": {"server": "fps.example.test:443"},
              "security": {
                "zero_rtt": {
                  "enabled": true,
                  "profile_id": "android-vpn-establish-test-v5",
                  "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
                  "server_public_key_base64": "$key"
                }
              },
              "tun": {"enabled": true, "name": "FPS Test", "mtu": 1280}
            }
        """.trimIndent()
    }
}
