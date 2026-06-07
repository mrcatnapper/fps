package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.config.CarrierProbeMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.Socket
import java.util.Base64

class HeadlessCarrierManagerTest {
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
          "carriers": [
            {"mode": "https_get", "endpoint": "origin.example.test:443", "path": "/ping", "interval_ms": 5000},
            {"mode": "wss", "endpoint": "[2001:db8::1]:9443", "path": "/stream", "interval_ms": 10000}
          ],
          "split_tunnel": {"allowed_uids": [10042]}
        }
        """.trimIndent(),
    )

    @Test
    fun startsOneRunnerPerCarrierAndProtectsBeforeConnect() {
        val hooks = FakeCarrierHooks()
        val factory = FakeCarrierTransportFactory(events = hooks.events)
        val manager = HeadlessCarrierManager(profile, hooks, factory)

        manager.start(nowMs = 1000)

        assertEquals(listOf("origin.example.test:443", "2001:db8::1:9443"), hooks.resolvedEndpoints)
        assertEquals(listOf(100, 101), hooks.protectedSockets)
        assertEquals(listOf(100, 101), factory.connectOrder)
        assertTrue(hooks.events.indexOf("protect:100") < hooks.events.indexOf("connect:100"))
        assertTrue(hooks.events.indexOf("protect:101") < hooks.events.indexOf("connect:101"))
        assertEquals(2, manager.statuses().count { it.state == CarrierRuntimeState.RUNNING })
    }

    @Test
    fun ticksPeriodicHttpsProbeByInterval() {
        val hooks = FakeCarrierHooks()
        val factory = FakeCarrierTransportFactory()
        val manager = HeadlessCarrierManager(profile, hooks, factory)

        manager.start(nowMs = 1000)
        manager.tick(nowMs = 1000)
        manager.tick(nowMs = 5999)
        manager.tick(nowMs = 6000)

        val httpsTransport = factory.created[0]
        assertEquals(CarrierProbeMode.HTTPS_GET, httpsTransport.plan.probe.mode)
        assertEquals(listOf(1000L, 6000L), httpsTransport.probeTimes)
        assertEquals(2, manager.statuses()[0].successfulProbes)
    }

    @Test
    fun reconnectsPersistentWssAfterProbeFailure() {
        val hooks = FakeCarrierHooks()
        val factory = FakeCarrierTransportFactory()
        val manager = HeadlessCarrierManager(
            profile,
            hooks,
            factory,
            initialBackoffMs = 1000,
            maxBackoffMs = 8000,
        )

        manager.start(nowMs = 0)
        factory.created[1].failNextProbe = "wss_disconnect"
        manager.tick(nowMs = 0)

        assertEquals(CarrierRuntimeState.BACKOFF, manager.statuses()[1].state)
        assertEquals("wss_disconnect", manager.statuses()[1].lastError)
        assertEquals(1, manager.statuses()[1].reconnects)

        manager.tick(nowMs = 999)
        assertEquals(1, factory.created.count { it.plan.id == 1 })

        manager.tick(nowMs = 1000)
        assertEquals(CarrierRuntimeState.RUNNING, manager.statuses()[1].state)
        assertEquals(2, factory.created.count { it.plan.id == 1 })
    }

    @Test
    fun resolveConnectAndProbeFailuresUseBackoffWithoutCrashing() {
        val resolveHooks = FakeCarrierHooks(resolveEndpoints = emptyList())
        val resolveManager = HeadlessCarrierManager(profile, resolveHooks, FakeCarrierTransportFactory(), initialBackoffMs = 500)

        resolveManager.start(nowMs = 10)

        assertEquals(CarrierRuntimeState.BACKOFF, resolveManager.statuses()[0].state)
        assertEquals("resolve_empty", resolveManager.statuses()[0].lastError)
        assertEquals(500L, resolveManager.statuses()[0].nextRetryDelayMs)

        val connectFactory = FakeCarrierTransportFactory(connectErrors = mutableMapOf(0 to "connect_failed"))
        val connectManager = HeadlessCarrierManager(profile, FakeCarrierHooks(), connectFactory, initialBackoffMs = 500)

        connectManager.start(nowMs = 10)

        assertEquals(CarrierRuntimeState.BACKOFF, connectManager.statuses()[0].state)
        assertEquals("connect_failed", connectManager.statuses()[0].lastError)
        assertEquals(1, connectManager.statuses()[0].attempts)
    }

    @Test
    fun socketProtectionFailureDoesNotConnect() {
        val hooks = FakeCarrierHooks(protectResult = false)
        val factory = FakeCarrierTransportFactory()
        val manager = HeadlessCarrierManager(profile, hooks, factory)

        manager.start(nowMs = 0)

        assertEquals(CarrierRuntimeState.BACKOFF, manager.statuses()[0].state)
        assertEquals("socket_protect_failed", manager.statuses()[0].lastError)
        assertTrue(factory.connectOrder.isEmpty())
        assertTrue(factory.created.all { it.closed })
    }

    @Test
    fun stopIsIdempotentAndClosesTransports() {
        val manager = HeadlessCarrierManager(profile, FakeCarrierHooks(), FakeCarrierTransportFactory())

        manager.start(nowMs = 0)
        val firstStop = manager.stop()
        val secondStop = manager.stop()

        assertEquals(2, firstStop.count { it.state == CarrierRuntimeState.STOPPED })
        assertEquals(2, secondStop.count { it.state == CarrierRuntimeState.STOPPED })
    }

    @Test
    fun statusDoesNotExposeSecrets() {
        val manager = HeadlessCarrierManager(profile, FakeCarrierHooks(), FakeCarrierTransportFactory())

        manager.start(nowMs = 0)
        val text = manager.statuses().toString()

        assertFalse(text.contains(profile.zeroRtt.clientUuid))
        assertFalse(text.contains(profile.zeroRtt.serverPublicKeyBase64))
        assertTrue(text.contains("successfulProbes"))
    }

    @Test
    fun controllerCanOwnCarrierRunnerLifecycle() {
        val hooks = FakeCarrierHooks()
        val factory = FakeCarrierTransportFactory(events = hooks.events)
        val controller = HeadlessVpnController(profile, hooks)

        assertEquals(VpnRuntimeState.WAITING_FOR_LEASE, controller.start())
        val started = controller.startCarrierRunners(factory, nowMs = 0)
        val stopped = controller.stopCarrierRunners()

        assertEquals(2, started.size)
        assertEquals(2, stopped.count { it.state == CarrierRuntimeState.STOPPED })
        assertEquals(VpnRuntimeState.WAITING_FOR_LEASE, controller.state)
    }
}

private class FakeCarrierHooks(
    private val protectResult: Boolean = true,
    private val resolveEndpoints: List<ResolvedEndpoint> = listOf(ResolvedEndpoint("203.0.113.10", 443)),
) : AndroidPlatformHooks {
    val events = mutableListOf<String>()
    val resolvedEndpoints = mutableListOf<String>()
    val protectedSockets = mutableListOf<Int>()

    override fun hasVpnPermission() = true

    override fun establishTun(profile: AndroidClientProfile, lease: TunLease): EstablishedTun {
        return EstablishedTun.borrowed(fd = 7, mtu = lease.mtu)
    }

    override fun protectSocket(fd: Int): Boolean {
        events += "protect:$fd"
        protectedSockets += fd
        return protectResult
    }

    override fun protectSocket(socket: Socket) = protectResult

    override fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint> {
        resolvedEndpoints += "$host:$port"
        return resolveEndpoints
    }

    override fun uidForFlow(flow: org.fpsproject.client.policy.TunFlowTuple) = -1
}

private class FakeCarrierTransportFactory(
    private val connectErrors: MutableMap<Int, String> = mutableMapOf(),
    private val events: MutableList<String> = mutableListOf(),
) : CarrierTransportFactory {
    val created = mutableListOf<FakeCarrierTransport>()
    val connectOrder = mutableListOf<Int>()

    override fun create(plan: CarrierRuntimePlan): CarrierTransport {
        val transport = FakeCarrierTransport(
            plan = plan,
            socketFd = 100 + created.size,
            connectErrors = connectErrors,
            connectOrder = connectOrder,
            events = events,
        )
        created += transport
        return transport
    }
}

private class FakeCarrierTransport(
    val plan: CarrierRuntimePlan,
    override val socketFd: Int,
    private val connectErrors: MutableMap<Int, String>,
    private val connectOrder: MutableList<Int>,
    private val events: MutableList<String>,
) : CarrierTransport {
    val probeTimes = mutableListOf<Long>()
    var failNextProbe: String? = null
    var closed = false

    override fun connect(endpoint: ResolvedEndpoint): CarrierTransportResult {
        events += "connect:$socketFd"
        connectOrder += socketFd
        return connectErrors.remove(plan.id)?.let { CarrierTransportResult.failure(it) } ?: CarrierTransportResult.success()
    }

    override fun probe(nowMs: Long): CarrierTransportResult {
        probeTimes += nowMs
        return failNextProbe?.let {
            failNextProbe = null
            CarrierTransportResult.failure(it)
        } ?: CarrierTransportResult.success()
    }

    override fun close() {
        closed = true
    }
}
