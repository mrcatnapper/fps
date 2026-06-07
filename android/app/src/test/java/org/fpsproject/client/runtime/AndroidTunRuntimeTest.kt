package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfileParser
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.Base64

class AndroidTunRuntimeTest {
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
          "tun": {"enabled": true, "name": "fpsc0", "mtu": 1400, "auto_configure": true}
        }
        """.trimIndent(),
    )
    private val lease = TunLease(
        clientIpv4 = 0x0a420002,
        serverIpv4 = 0x0a420001,
        prefixLength = 30,
        mtu = 1280,
    )

    @Test
    fun buildsLeaseOnlyTunPlan() {
        val plan = buildAndroidTunPlan(profile, lease)

        assertEquals("fpsc0", plan.sessionName)
        assertEquals(1280, plan.mtu)
        assertEquals(listOf(Ipv4Cidr("10.66.0.2", 30)), plan.addresses)
        assertEquals(listOf(Ipv4Cidr("10.66.0.0", 30)), plan.routes)
        assertEquals(emptyList<String>(), plan.dnsServers)
    }

    @Test
    fun establishesTunThroughBuilder() {
        val builder = FakeVpnTunnelBuilder(FakeTunHandle(fd = 77))
        val established = VpnTunEstablisher { builder }.establish(profile, lease)

        assertEquals(EstablishedTun(fd = 77, mtu = 1280), established)
        assertEquals(
            listOf(
                "session:fpsc0",
                "mtu:1280",
                "address:10.66.0.2/30",
                "route:10.66.0.0/30",
                "establish",
            ),
            builder.calls,
        )
    }

    @Test
    fun establishFailureReturnsNull() {
        val builder = FakeVpnTunnelBuilder(handle = null)

        assertNull(VpnTunEstablisher { builder }.establish(profile, lease))
        assertEquals("establish", builder.calls.last())
    }

    @Test
    fun controllerStopClosesEstablishedTunOnce() {
        val handle = FakeTunHandle(fd = 42)
        val hooks = FakeTunHooks(establishedTun = EstablishedTun(fd = handle.fd, mtu = 1280, closeAction = handle::close))
        val controller = HeadlessVpnController(profile, hooks)

        controller.start()
        controller.onLeaseReceived(lease)
        controller.stop()
        controller.stop()

        assertEquals(1, handle.closeCount)
    }
}

private class FakeVpnTunnelBuilder(private val handle: TunHandle?) : VpnTunnelBuilder {
    val calls = mutableListOf<String>()

    override fun setSession(name: String) {
        calls += "session:$name"
    }

    override fun setMtu(mtu: Int) {
        calls += "mtu:$mtu"
    }

    override fun addAddress(address: String, prefixLength: Int) {
        calls += "address:$address/$prefixLength"
    }

    override fun addRoute(address: String, prefixLength: Int) {
        calls += "route:$address/$prefixLength"
    }

    override fun addDnsServer(address: String) {
        calls += "dns:$address"
    }

    override fun establish(): TunHandle? {
        calls += "establish"
        return handle
    }
}

private class FakeTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}

private class FakeTunHooks(
    private val establishedTun: EstablishedTun?,
) : AndroidPlatformHooks {
    override fun hasVpnPermission() = true

    override fun establishTun(profile: org.fpsproject.client.config.AndroidClientProfile, lease: TunLease) = establishedTun

    override fun protectSocket(fd: Int) = true

    override fun protectSocket(socket: java.net.Socket) = true

    override fun resolveOnUnderlyingNetwork(host: String, port: Int) = listOf(ResolvedEndpoint("203.0.113.10", port))

    override fun uidForFlow(flow: org.fpsproject.client.policy.TunFlowTuple) = -1
}
