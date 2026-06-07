package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.config.CarrierProbeMode
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.TunProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Base64

class HeadlessVpnControllerTest {
    private val profile = AndroidClientProfileParser.parse(
        """
        {
          "network": {"server": "fps.example.test:443"},
          "security": {
            "zero_rtt": {
              "enabled": true,
              "profile_id": "android-test-v5",
              "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
              "server_public_key_base64": "${Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })}"
            }
          },
          "tun": {"enabled": true, "name": "fpsc0", "mtu": 1280, "auto_configure": true},
          "carriers": [
            {"mode": "https_get", "endpoint": "origin.example.test:443", "path": "/ping", "interval_ms": 5000},
            {"mode": "wss", "endpoint": "[2001:db8::1]:9443", "path": "/stream", "interval_ms": 10000}
          ],
          "split_tunnel": {"allowed_uids": [10042]}
        }
        """.trimIndent(),
    )

    @Test
    fun startRequiresVpnPermission() {
        val hooks = FakeAndroidPlatformHooks(vpnPermissionGranted = false)
        val controller = HeadlessVpnController(profile, hooks)

        assertEquals(VpnRuntimeState.NEEDS_VPN_PERMISSION, controller.start())
        assertEquals(VpnRuntimeState.NEEDS_VPN_PERMISSION, controller.state)
        assertEquals(0, hooks.establishTunCalls)
    }

    @Test
    fun waitsForLeaseBeforeEstablishingTun() {
        val hooks = FakeAndroidPlatformHooks(vpnPermissionGranted = true)
        val controller = HeadlessVpnController(profile, hooks)

        assertEquals(VpnRuntimeState.WAITING_FOR_LEASE, controller.start())
        assertEquals(0, hooks.establishTunCalls)

        val lease = TunLease(clientIpv4 = 0x0a420002, serverIpv4 = 0x0a420001, prefixLength = 30, mtu = 1280)
        assertEquals(VpnRuntimeState.RUNNING, controller.onLeaseReceived(lease))
        assertEquals(1, hooks.establishTunCalls)
    }

    @Test
    fun stopIsIdempotent() {
        val controller = HeadlessVpnController(profile, FakeAndroidPlatformHooks())

        assertEquals(VpnRuntimeState.STOPPED, controller.stop())
        assertEquals(VpnRuntimeState.STOPPED, controller.stop())
    }

    @Test
    fun failedTunEstablishmentMovesToFailed() {
        val hooks = FakeAndroidPlatformHooks(establishTunResult = null)
        val controller = HeadlessVpnController(profile, hooks)
        val lease = TunLease(clientIpv4 = 0x0a420002, serverIpv4 = 0x0a420001, prefixLength = 30, mtu = 1280)

        controller.start()

        assertEquals(VpnRuntimeState.FAILED, controller.onLeaseReceived(lease))
        assertEquals("tun_establish_failed", controller.lastError)
    }

    @Test
    fun carrierSocketMustBeProtectedBeforeConnect() {
        val hooks = FakeAndroidPlatformHooks(protectSocketResult = true)
        val controller = HeadlessVpnController(profile, hooks)

        assertTrue(controller.prepareCarrierSocket(42))
        assertEquals(listOf(42), hooks.protectedSockets)
    }

    @Test
    fun exposesCarrierRuntimePlansFromProfile() {
        val controller = HeadlessVpnController(profile, FakeAndroidPlatformHooks())

        val plans = controller.carrierPlans()

        assertEquals(2, plans.size)
        assertEquals(0, plans[0].id)
        assertEquals(CarrierProbeMode.HTTPS_GET, plans[0].probe.mode)
        assertEquals("/ping", plans[0].probe.path)
        assertEquals(1, plans[1].id)
        assertEquals(CarrierProbeMode.WSS, plans[1].probe.mode)
    }

    @Test
    fun carrierEndpointResolutionUsesPlatformHook() {
        val hooks = FakeAndroidPlatformHooks()
        val controller = HeadlessVpnController(profile, hooks)

        assertEquals(listOf(ResolvedEndpoint("203.0.113.20", 443)), controller.resolveCarrierEndpoint(controller.carrierPlans()[0]))
        assertEquals(listOf("origin.example.test:443"), hooks.resolvedEndpoints)
    }

    @Test
    fun serverEndpointResolutionUsesPlatformHook() {
        val hooks = FakeAndroidPlatformHooks()
        val controller = HeadlessVpnController(profile, hooks)

        assertEquals(listOf(ResolvedEndpoint("203.0.113.10", 443)), controller.resolveServerEndpoint())
        assertEquals(listOf("fps.example.test:443"), hooks.resolvedEndpoints)
    }

    @Test
    fun failedCarrierSocketProtectionFailsClosed() {
        val hooks = FakeAndroidPlatformHooks(protectSocketResult = false)
        val controller = HeadlessVpnController(profile, hooks)

        assertFalse(controller.prepareCarrierSocket(42))
        assertEquals(VpnRuntimeState.FAILED, controller.state)
        assertEquals("socket_protect_failed", controller.lastError)
    }

    @Test
    fun platformUidResolverDelegatesToHooks() {
        val flow = TunFlowTuple(TunProtocol.TCP, 0x0a420002, 53000, 0x5db8d822, 443)
        val hooks = FakeAndroidPlatformHooks(uidForFlowResult = 10042)

        assertEquals(10042, PlatformUidResolver(hooks).ownerUidFor(flow))
    }

    @Test
    fun splitTunnelPolicyAllowsConfiguredUid() {
        val flow = TunFlowTuple(TunProtocol.TCP, 0x0a420002, 53000, 0x5db8d822, 443)
        val controller = HeadlessVpnController(profile, FakeAndroidPlatformHooks(uidForFlowResult = 10042))

        assertEquals(SplitTunnelDecision.ALLOW, controller.policyDecision(flow))
    }

    @Test
    fun splitTunnelPolicyDropsUnknownAndNonAllowlistedFlows() {
        val flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x08080808, 53)
        val controller = HeadlessVpnController(profile, FakeAndroidPlatformHooks(uidForFlowResult = 10043))

        assertEquals(SplitTunnelDecision.DROP, controller.policyDecision(null))
        assertEquals(SplitTunnelDecision.DROP, controller.policyDecision(flow))
    }
}

private class FakeAndroidPlatformHooks(
    private val vpnPermissionGranted: Boolean = true,
    private val establishTunResult: EstablishedTun? = EstablishedTun(fd = 7, mtu = 1280),
    private val protectSocketResult: Boolean = true,
    private val uidForFlowResult: Int = -1,
) : AndroidPlatformHooks {
    var establishTunCalls = 0
    val protectedSockets = mutableListOf<Int>()
    val resolvedEndpoints = mutableListOf<String>()

    override fun hasVpnPermission() = vpnPermissionGranted

    override fun establishTun(profile: org.fpsproject.client.config.AndroidClientProfile, lease: TunLease): EstablishedTun? {
        establishTunCalls += 1
        return establishTunResult
    }

    override fun protectSocket(fd: Int): Boolean {
        protectedSockets += fd
        return protectSocketResult
    }

    override fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint> {
        resolvedEndpoints += "$host:$port"
        val address = if (host == "fps.example.test") "203.0.113.10" else "203.0.113.20"
        return listOf(ResolvedEndpoint(address, port))
    }

    override fun uidForFlow(flow: TunFlowTuple) = uidForFlowResult
}
