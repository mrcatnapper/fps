package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.ResolvedEndpoint
import org.fpsproject.client.runtime.TunHandle
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnRuntimeState
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.TunProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.Socket
import java.util.Base64

class HeadlessNativeVpnRuntimeTest {
    private val uuid = "123e4567-e89b-42d3-a456-426614174000"
    private val key = Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })
    private val profileJson = """
        {
          "network": {"server": "fps.example.test:443"},
          "security": {
            "zero_rtt": {
              "enabled": true,
              "profile_id": "android-test-v5",
              "client_uuid": "$uuid",
              "server_public_key_base64": "$key"
            }
          },
          "tun": {"enabled": true, "name": "fpsc0", "mtu": 1280, "auto_configure": true},
          "split_tunnel": {"allowed_uids": [10042]}
        }
    """.trimIndent()
    private val lease = TunLease(clientIpv4 = 0x0a420002, serverIpv4 = 0x0a420001, prefixLength = 30, mtu = 1280)

    @Test
    fun startStartsNativeRuntimeAndWaitsForLease() {
        val backend = FakeCoordinatorNativeBackend()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(), backend)

        assertEquals(VpnRuntimeState.WAITING_FOR_LEASE, runtime.start())
        assertEquals(1, backend.createdProfiles.size)
        assertEquals(profileJson, backend.createdProfiles.single().second)
        assertEquals(listOf(1L), backend.startedHandles)
        assertTrue(runtime.snapshot().native.alive)
        assertTrue(runtime.snapshot().native.started)
        assertFalse(runtime.snapshot().vpn.tun.fdPresent)
    }

    @Test
    fun startDoesNotStartNativeRuntimeWhenVpnPermissionIsMissing() {
        val backend = FakeCoordinatorNativeBackend()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(vpnPermissionGranted = false), backend)

        assertEquals(VpnRuntimeState.NEEDS_VPN_PERMISSION, runtime.start())

        assertEquals(emptyList<Long>(), backend.startedHandles)
        assertFalse(runtime.snapshot().native.started)
    }

    @Test
    fun stopBeforeLeaseStopsNativeExecutorAndDoesNotRequireTun() {
        val backend = FakeCoordinatorNativeBackend()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(), backend)

        runtime.start()
        val state = runtime.stop()
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.STOPPED, state)
        assertEquals(listOf(1L), backend.stoppedHandles)
        assertEquals(listOf(1L), backend.stoppedTunPumps)
        assertFalse(snapshot.vpn.tun.fdPresent)
        assertFalse(snapshot.native.started)
        assertFalse(snapshot.native.workerThreadRunning)
        assertFalse(snapshot.native.tunPumpRunning)
    }

    @Test
    fun leaseEstablishesTunAndAttachesDuplicateFdToNativeRuntime() {
        val backend = FakeCoordinatorNativeBackend()
        val hooks = FakeAndroidHooks(establishedTun = EstablishedTun.borrowed(fd = 77, mtu = 1280))
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val state = runtime.onLeaseReceived(lease)
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.RUNNING, state)
        assertEquals(1, hooks.establishTunCalls)
        assertEquals(listOf(Triple(1L, 77, 1280)), backend.attachedTun)
        assertEquals(listOf(1L), backend.startedTunPumps)
        assertTrue(snapshot.vpn.tun.fdPresent)
        assertTrue(snapshot.native.tunAttached)
        assertEquals(TUN_FD_OWNERSHIP_OWNED_DUPLICATE, snapshot.native.tunFdOwnership)
        assertTrue(snapshot.native.tunPumpRunning)
    }

    @Test
    fun nativeTunAttachFailureFailsClosedAndClosesEstablishedTun() {
        val backend = FakeCoordinatorNativeBackend(attachResult = AttachResult.FAIL_INVALID_FD)
        val tunHandle = CountingTunHandle(fd = 77)
        val hooks = FakeAndroidHooks(establishedTun = EstablishedTun.owned(fd = tunHandle.fd, mtu = 1280, handle = tunHandle))
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val state = runtime.onLeaseReceived(lease)
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.FAILED, state)
        assertEquals("native_tun_attach_failed", snapshot.vpn.lastError)
        assertFalse(snapshot.vpn.tun.fdPresent)
        assertFalse(snapshot.native.tunAttached)
        assertEquals("invalid_tun_fd", snapshot.native.lastError)
        assertEquals(1, tunHandle.closeCount)
    }

    @Test
    fun nativeTunPumpStartFailureFailsClosedAndClosesEstablishedTun() {
        val backend = FakeCoordinatorNativeBackend(pumpResult = PumpResult.FAIL_TUN_NOT_ATTACHED)
        val tunHandle = CountingTunHandle(fd = 77)
        val hooks = FakeAndroidHooks(establishedTun = EstablishedTun.owned(fd = tunHandle.fd, mtu = 1280, handle = tunHandle))
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val state = runtime.onLeaseReceived(lease)
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.FAILED, state)
        assertEquals("native_tun_pump_start_failed", snapshot.vpn.lastError)
        assertFalse(snapshot.vpn.tun.fdPresent)
        assertFalse(snapshot.native.tunPumpRunning)
        assertEquals("tun_not_attached", snapshot.native.lastError)
        assertEquals(1, tunHandle.closeCount)
    }

    @Test
    fun stopIsIdempotentAndCloseReleasesNativeHandle() {
        val backend = FakeCoordinatorNativeBackend()
        val tunHandle = CountingTunHandle(fd = 77)
        val hooks = FakeAndroidHooks(establishedTun = EstablishedTun.owned(fd = tunHandle.fd, mtu = 1280, handle = tunHandle))
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        runtime.onLeaseReceived(lease)
        runtime.stop()
        runtime.stop()
        runtime.close()
        runtime.close()

        assertEquals(listOf(1L, 1L, 1L), backend.stoppedHandles)
        assertEquals(listOf(1L, 1L, 1L), backend.stoppedTunPumps)
        assertEquals(listOf(1L), backend.closedHandles)
        assertEquals(1, tunHandle.closeCount)
        assertEquals(VpnRuntimeState.STOPPED, runtime.snapshot().vpn.state)
        assertEquals("runtime_closed", runtime.snapshot().native.lastError)
    }

    @Test
    fun snapshotsDoNotExposeProfileOrIdentityMaterial() {
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(), FakeCoordinatorNativeBackend())

        runtime.start()
        val text = runtime.snapshot().toString()

        assertFalse(text.contains(profileJson))
        assertFalse(text.contains(uuid))
        assertFalse(text.contains(key))
    }

    @Test
    fun applyPendingTunPolicyUsesKotlinUidPolicyAndCompletesNativePackets() {
        val backend = FakeCoordinatorNativeBackend(
            initialPolicyPackets = listOf(
                NativeTunPolicyPacket(
                    packetId = 101,
                    packetSize = 28,
                    flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x5db8d822, 443),
                ),
                NativeTunPolicyPacket(
                    packetId = 102,
                    packetSize = 40,
                    flow = TunFlowTuple(TunProtocol.TCP, 0x0a420002, 53001, 0x5db8d822, 8443),
                ),
            ),
        )
        val hooks = FakeAndroidHooks(uidForFlow = { flow ->
            if (flow.destinationPort == 443) {
                10042
            } else {
                10043
            }
        })
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val snapshot = runtime.applyPendingTunPolicy(maxPackets = 8)

        assertEquals(listOf(101L to SplitTunnelDecision.ALLOW, 102L to SplitTunnelDecision.DROP), backend.completedPolicy)
        assertEquals(1L, snapshot.tunPolicyAllowed)
        assertEquals(1L, snapshot.tunPolicyDropped)
        assertEquals(0L, snapshot.tunPolicyPending)
        assertEquals(0L, snapshot.tunPolicyInFlight)
    }

    @Test
    fun startNativeCarrierResolvesProtectsAndCompletesNativeConnect() {
        val backend = FakeCoordinatorNativeBackend()
        val hooks = FakeAndroidHooks()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val endpoint = runtime.resolveServerEndpoint().single()
        val snapshot = runtime.startNativeCarrier(endpoint)

        assertEquals(listOf("fps.example.test:443"), hooks.resolvedEndpoints)
        assertEquals(listOf(77), hooks.protectedFds)
        assertEquals(listOf(Triple(1L, "203.0.113.10", 443)), backend.preparedRawCarriers)
        assertEquals(listOf(1L to true), backend.completedRawCarrierProtection)
        assertTrue(snapshot.rawCarrierActive)
        assertEquals(1L, snapshot.rawCarrierConnectAttempted)
        assertEquals(1L, snapshot.rawCarrierConnectSucceeded)
        assertEquals(null, snapshot.lastError)
    }

    @Test
    fun startNativeCarrierAbortsNativeConnectWhenProtectFails() {
        val backend = FakeCoordinatorNativeBackend()
        val hooks = FakeAndroidHooks(protectSocketResult = false)
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val snapshot = runtime.startNativeCarrier(ResolvedEndpoint("203.0.113.10", 443))

        assertEquals(listOf(77), hooks.protectedFds)
        assertEquals(listOf(1L to false), backend.completedRawCarrierProtection)
        assertFalse(snapshot.rawCarrierActive)
        assertEquals("socket_protect_failed", snapshot.lastError)
        assertEquals(VpnRuntimeState.FAILED, runtime.state)
        assertEquals("socket_protect_failed", runtime.lastError)
    }

    @Test
    fun startNativeCarrierFailsClosedWhenNativePrepareFails() {
        val backend = FakeCoordinatorNativeBackend()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(), backend)

        val snapshot = runtime.startNativeCarrier(ResolvedEndpoint("203.0.113.10", 443))

        assertEquals(listOf(Triple(1L, "203.0.113.10", 443)), backend.preparedRawCarriers)
        assertEquals(emptyList<Pair<Long, Boolean>>(), backend.completedRawCarrierProtection)
        assertEquals(-1, snapshot.rawCarrierProtectFd)
        assertEquals("runtime_stopped", snapshot.lastError)
        assertEquals(VpnRuntimeState.FAILED, runtime.state)
        assertEquals("runtime_stopped", runtime.lastError)
    }

    @Test
    fun nativeLeaseEventRunsExistingLeaseBeforeTunPath() {
        val backend = FakeCoordinatorNativeBackend(initialNativeEvents = listOf(leaseEvent()))
        val hooks = FakeAndroidHooks(establishedTun = EstablishedTun.borrowed(fd = 77, mtu = 1280))
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val state = runtime.applyNativeEvents()
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.RUNNING, state)
        assertEquals(1, hooks.establishTunCalls)
        assertEquals(listOf(Triple(1L, 77, 1280)), backend.attachedTun)
        assertEquals(listOf(1L), backend.startedTunPumps)
        assertTrue(snapshot.vpn.tun.fdPresent)
        assertTrue(snapshot.native.tunAttached)
        assertTrue(snapshot.native.tunPumpRunning)
        assertEquals(1, backend.drainNativeEventsCalls)
    }

    @Test
    fun nativeAuthFailureEventFailsClosedWithoutTun() {
        val backend = FakeCoordinatorNativeBackend(
            initialNativeEvents = listOf(NativeRuntimeEvent(type = NATIVE_EVENT_CARRIER_AUTH_FAILED, error = "carrier_auth_failed")),
        )
        val hooks = FakeAndroidHooks()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, hooks, backend)

        runtime.start()
        val state = runtime.applyNativeEvents()
        val snapshot = runtime.snapshot()

        assertEquals(VpnRuntimeState.FAILED, state)
        assertEquals("carrier_auth_failed", snapshot.vpn.lastError)
        assertEquals(0, hooks.establishTunCalls)
        assertFalse(snapshot.vpn.tun.fdPresent)
    }

    @Test
    fun clientAuthSmokeDelegatesToNativeBackend() {
        val backend = FakeCoordinatorNativeBackend()
        val runtime = HeadlessNativeVpnRuntime.create(profileJson, FakeAndroidHooks(), backend)

        runtime.start()
        val succeeded = runtime.runClientAuthSmokeForTest()
        val failed = runtime.runClientAuthSmokeForTest(tamperServerAccept = true)

        assertEquals(listOf(1L to false, 1L to true), backend.clientAuthSmokeCalls)
        assertEquals(1L, succeeded.carrierAuthSucceeded)
        assertEquals(1L, failed.carrierAuthFailed)
    }
}

private fun leaseEvent() = NativeRuntimeEvent(
    type = NATIVE_EVENT_LEASE_RECEIVED,
    clientIpv4 = 0x0a420002,
    serverIpv4 = 0x0a420001,
    prefixLength = 30,
    mtu = 1280,
)

private fun coordinatorSnapshot(
    alive: Boolean = true,
    started: Boolean = false,
    workerThreadRunning: Boolean = false,
    tunAttached: Boolean = false,
    tunPumpRunning: Boolean = false,
    tunFd: Int = -1,
    tunMtu: Int = 0,
    tunFdOwnership: String? = null,
    tunPacketsRead: Long = 0,
    tunBytesRead: Long = 0,
    tunPacketsParsed: Long = 0,
    tunPacketsDropped: Long = 0,
    tunLastDropReason: String? = null,
    tunPolicyPending: Long = 0,
    tunPolicyInFlight: Long = 0,
    tunPolicyAllowed: Long = 0,
    tunPolicyDropped: Long = 0,
    tunPolicyQueueFull: Long = 0,
    tunCovertEnqueueAttempted: Long = 0,
    tunCovertEnqueueAccepted: Long = 0,
    tunCovertEnqueueRejected: Long = 0,
    commandsPosted: Long = 0,
    commandsCompleted: Long = 0,
    rawCarrierProtectFd: Int = -1,
    rawCarrierConnecting: Boolean = false,
    rawCarrierActive: Boolean = false,
    rawCarrierConnectAttempted: Long = 0,
    rawCarrierConnectSucceeded: Long = 0,
    rawCarrierConnectFailed: Long = 0,
    carrierAuthConfigured: Boolean = false,
    carrierAuthAttempted: Long = 0,
    carrierAuthSucceeded: Long = 0,
    carrierAuthFailed: Long = 0,
    carrierLeaseReceived: Long = 0,
    lastError: String? = null,
) = NativeRuntimeSnapshot(
    alive = alive,
    started = started,
    workerThreadRunning = workerThreadRunning,
    tunAttached = tunAttached,
    tunPumpRunning = tunPumpRunning,
    tunFd = tunFd,
    tunMtu = tunMtu,
    tunFdOwnership = tunFdOwnership,
    tunPacketsRead = tunPacketsRead,
    tunBytesRead = tunBytesRead,
    tunPacketsParsed = tunPacketsParsed,
    tunPacketsDropped = tunPacketsDropped,
    tunLastDropReason = tunLastDropReason,
    tunPolicyPending = tunPolicyPending,
    tunPolicyInFlight = tunPolicyInFlight,
    tunPolicyAllowed = tunPolicyAllowed,
    tunPolicyDropped = tunPolicyDropped,
    tunPolicyQueueFull = tunPolicyQueueFull,
    tunCovertEnqueueAttempted = tunCovertEnqueueAttempted,
    tunCovertEnqueueAccepted = tunCovertEnqueueAccepted,
    tunCovertEnqueueRejected = tunCovertEnqueueRejected,
    commandsPosted = commandsPosted,
    commandsCompleted = commandsCompleted,
    rawCarrierProtectFd = rawCarrierProtectFd,
    rawCarrierConnecting = rawCarrierConnecting,
    rawCarrierActive = rawCarrierActive,
    rawCarrierConnectAttempted = rawCarrierConnectAttempted,
    rawCarrierConnectSucceeded = rawCarrierConnectSucceeded,
    rawCarrierConnectFailed = rawCarrierConnectFailed,
    carrierAuthConfigured = carrierAuthConfigured,
    carrierAuthAttempted = carrierAuthAttempted,
    carrierAuthSucceeded = carrierAuthSucceeded,
    carrierAuthFailed = carrierAuthFailed,
    carrierLeaseReceived = carrierLeaseReceived,
    lastError = lastError,
)

private enum class AttachResult {
    SUCCESS,
    FAIL_INVALID_FD,
}

private enum class PumpResult {
    SUCCESS,
    FAIL_TUN_NOT_ATTACHED,
}

private class FakeCoordinatorNativeBackend(
    private val attachResult: AttachResult = AttachResult.SUCCESS,
    private val pumpResult: PumpResult = PumpResult.SUCCESS,
    initialPolicyPackets: List<NativeTunPolicyPacket> = emptyList(),
    initialNativeEvents: List<NativeRuntimeEvent> = emptyList(),
) : FpsNativeBackend {
    private var nextHandle = 1L
    val createdProfiles = mutableListOf<Pair<Long, String>>()
    val startedHandles = mutableListOf<Long>()
    val stoppedHandles = mutableListOf<Long>()
    val closedHandles = mutableListOf<Long>()
    val attachedTun = mutableListOf<Triple<Long, Int, Int>>()
    val startedTunPumps = mutableListOf<Long>()
    val stoppedTunPumps = mutableListOf<Long>()
    val completedPolicy = mutableListOf<Pair<Long, SplitTunnelDecision>>()
    val preparedRawCarriers = mutableListOf<Triple<Long, String, Int>>()
    val completedRawCarrierProtection = mutableListOf<Pair<Long, Boolean>>()
    val stoppedRawCarriers = mutableListOf<Long>()
    val configuredAuth = mutableListOf<CoordinatorAuthConfigCall>()
    val clientAuthSmokeCalls = mutableListOf<Pair<Long, Boolean>>()
    var drainNativeEventsCalls = 0
    private val snapshots = mutableMapOf<Long, NativeRuntimeSnapshot>()
    private val pendingPolicy = ArrayDeque(initialPolicyPackets)
    private val inFlightPolicy = mutableSetOf<Long>()
    private val nativeEvents = ArrayDeque(initialNativeEvents)

    override fun createRuntime(profileText: String): Long {
        val handle = nextHandle++
        createdProfiles += handle to profileText
        snapshots[handle] = coordinatorSnapshot()
        return handle
    }

    override fun closeRuntime(handle: Long) {
        closedHandles += handle
        snapshots.remove(handle)
    }

    override fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot {
        return snapshots[handle] ?: coordinatorSnapshot(
            alive = false,
            lastError = "runtime_closed",
        )
    }

    override fun startTunPump(handle: Long): NativeRuntimeSnapshot {
        startedTunPumps += handle
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        val snapshot = when (pumpResult) {
            PumpResult.SUCCESS -> current.copy(
                tunPumpRunning = true,
                lastError = null,
            )
            PumpResult.FAIL_TUN_NOT_ATTACHED -> current.copy(
                tunPumpRunning = false,
                lastError = "tun_not_attached",
            )
        }
        snapshots[handle] = snapshot
        return snapshot
    }

    override fun stopTunPump(handle: Long): NativeRuntimeSnapshot {
        stoppedTunPumps += handle
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            tunPumpRunning = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun startRuntime(handle: Long): NativeRuntimeSnapshot {
        startedHandles += handle
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            started = true,
            workerThreadRunning = true,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun stopRuntime(handle: Long): NativeRuntimeSnapshot {
        stoppedHandles += handle
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            started = false,
            workerThreadRunning = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun postNoopCommand(handle: Long): NativeRuntimeSnapshot {
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            return current.copy(lastError = "runtime_stopped").also { snapshots[handle] = it }
        }
        return current.copy(
            commandsPosted = current.commandsPosted + 1,
            commandsCompleted = current.commandsCompleted + 1,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun attachTunFdOwnedDuplicate(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot {
        attachedTun += Triple(handle, fd, mtu)
        val snapshot = when (attachResult) {
            AttachResult.SUCCESS -> (snapshots[handle] ?: coordinatorSnapshot()).copy(
                tunAttached = true,
                tunFd = fd,
                tunMtu = mtu,
                tunFdOwnership = TUN_FD_OWNERSHIP_OWNED_DUPLICATE,
                lastError = null,
            )
            AttachResult.FAIL_INVALID_FD -> (snapshots[handle] ?: coordinatorSnapshot()).copy(
                tunAttached = false,
                tunPumpRunning = false,
                tunFd = -1,
                tunMtu = 0,
                tunFdOwnership = null,
                lastError = "invalid_tun_fd",
            )
        }
        snapshots[handle] = snapshot
        return snapshot
    }

    override fun drainTunPolicyPackets(handle: Long, maxPackets: Int): List<NativeTunPolicyPacket> {
        val current = snapshots[handle] ?: return emptyList()
        val out = mutableListOf<NativeTunPolicyPacket>()
        repeat(maxPackets.coerceAtLeast(0)) {
            val packet = pendingPolicy.removeFirstOrNull() ?: return@repeat
            out += packet
            inFlightPolicy += packet.packetId
        }
        snapshots[handle] = current.copy(
            tunPolicyPending = pendingPolicy.size.toLong(),
            tunPolicyInFlight = inFlightPolicy.size.toLong(),
        )
        return out
    }

    override fun completeTunPolicyPacket(handle: Long, packetId: Long, decision: SplitTunnelDecision): NativeRuntimeSnapshot {
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        if (!inFlightPolicy.remove(packetId)) {
            return current.copy(lastError = "unknown_tun_policy_packet_id").also { snapshots[handle] = it }
        }
        completedPolicy += packetId to decision
        if (decision == SplitTunnelDecision.ALLOW) {
            val snapshot = current.copy(
                tunPolicyPending = pendingPolicy.size.toLong(),
                tunPolicyInFlight = inFlightPolicy.size.toLong(),
                tunPolicyAllowed = current.tunPolicyAllowed + 1,
                tunPacketsDropped = current.tunPacketsDropped + 1,
                tunLastDropReason = "no_carrier_transport",
                tunCovertEnqueueAttempted = current.tunCovertEnqueueAttempted + 1,
                tunCovertEnqueueRejected = current.tunCovertEnqueueRejected + 1,
                lastError = "no_carrier_transport",
            )
            snapshots[handle] = snapshot
            return snapshot
        }
        val snapshot = current.copy(
            tunPolicyPending = pendingPolicy.size.toLong(),
            tunPolicyInFlight = inFlightPolicy.size.toLong(),
            tunPolicyDropped = current.tunPolicyDropped + 1,
            tunPacketsDropped = current.tunPacketsDropped + 1,
            tunLastDropReason = "tun_policy_drop",
            lastError = null,
        )
        snapshots[handle] = snapshot
        return snapshot
    }

    override fun prepareRawCarrierSocket(handle: Long, address: String, port: Int): NativeRuntimeSnapshot {
        preparedRawCarriers += Triple(handle, address, port)
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            return current.copy(rawCarrierProtectFd = -1, lastError = "runtime_stopped").also { snapshots[handle] = it }
        }
        return current.copy(
            rawCarrierProtectFd = 77,
            rawCarrierConnecting = false,
            rawCarrierActive = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun completeRawCarrierProtection(handle: Long, protectAllowed: Boolean): NativeRuntimeSnapshot {
        completedRawCarrierProtection += handle to protectAllowed
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        if (current.rawCarrierProtectFd < 0) {
            return current.copy(lastError = "raw_carrier_not_prepared").also { snapshots[handle] = it }
        }
        if (!protectAllowed) {
            return current.copy(
                rawCarrierProtectFd = -1,
                rawCarrierConnecting = false,
                rawCarrierActive = false,
                rawCarrierConnectFailed = current.rawCarrierConnectFailed + 1,
                lastError = "socket_protect_failed",
            ).also { snapshots[handle] = it }
        }
        return current.copy(
            rawCarrierProtectFd = -1,
            rawCarrierConnecting = false,
            rawCarrierActive = true,
            rawCarrierConnectAttempted = current.rawCarrierConnectAttempted + 1,
            rawCarrierConnectSucceeded = current.rawCarrierConnectSucceeded + 1,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun stopRawCarrier(handle: Long): NativeRuntimeSnapshot {
        stoppedRawCarriers += handle
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            rawCarrierProtectFd = -1,
            rawCarrierConnecting = false,
            rawCarrierActive = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun configureClientAuth(handle: Long, profileId: String, clientUuid: String, serverPublicKeyBase64: String): NativeRuntimeSnapshot {
        configuredAuth += CoordinatorAuthConfigCall(handle, profileId, clientUuid, serverPublicKeyBase64)
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            carrierAuthConfigured = true,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun runClientAuthSmokeForTest(handle: Long, tamperServerAccept: Boolean): NativeRuntimeSnapshot {
        clientAuthSmokeCalls += handle to tamperServerAccept
        val current = snapshots[handle] ?: return coordinatorSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            return current.copy(
                carrierAuthAttempted = current.carrierAuthAttempted + 1,
                carrierAuthFailed = current.carrierAuthFailed + 1,
                lastError = "runtime_stopped",
            ).also { snapshots[handle] = it }
        }
        if (tamperServerAccept) {
            nativeEvents += NativeRuntimeEvent(type = NATIVE_EVENT_CARRIER_AUTH_FAILED, error = "carrier_auth_failed")
            return current.copy(
                carrierAuthAttempted = current.carrierAuthAttempted + 1,
                carrierAuthFailed = current.carrierAuthFailed + 1,
                lastError = "carrier_auth_failed",
            ).also { snapshots[handle] = it }
        }
        nativeEvents += leaseEvent()
        return current.copy(
            carrierAuthAttempted = current.carrierAuthAttempted + 1,
            carrierAuthSucceeded = current.carrierAuthSucceeded + 1,
            carrierLeaseReceived = current.carrierLeaseReceived + 1,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun drainNativeEvents(handle: Long, maxEvents: Int): List<NativeRuntimeEvent> {
        drainNativeEventsCalls += 1
        if (!snapshots.containsKey(handle) || maxEvents <= 0) {
            return emptyList()
        }
        val out = mutableListOf<NativeRuntimeEvent>()
        repeat(maxEvents) {
            out += nativeEvents.removeFirstOrNull() ?: return@repeat
        }
        return out
    }

}

private data class CoordinatorAuthConfigCall(
    val handle: Long,
    val profileId: String,
    val clientUuid: String,
    val serverPublicKeyBase64: String,
)

private class FakeAndroidHooks(
    private val vpnPermissionGranted: Boolean = true,
    private val establishedTun: EstablishedTun? = EstablishedTun.borrowed(fd = 7, mtu = 1280),
    private val uidForFlow: (TunFlowTuple) -> Int = { -1 },
    private val protectSocketResult: Boolean = true,
) : AndroidPlatformHooks {
    var establishTunCalls = 0
    val protectedFds = mutableListOf<Int>()
    val resolvedEndpoints = mutableListOf<String>()

    override fun hasVpnPermission() = vpnPermissionGranted

    override fun establishTun(profile: AndroidClientProfile, lease: TunLease): EstablishedTun? {
        establishTunCalls += 1
        return establishedTun
    }

    override fun protectSocket(fd: Int): Boolean {
        protectedFds += fd
        return protectSocketResult
    }

    override fun protectSocket(socket: Socket) = protectSocketResult

    override fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint> {
        resolvedEndpoints += "$host:$port"
        return listOf(ResolvedEndpoint("203.0.113.10", port))
    }

    override fun uidForFlow(flow: TunFlowTuple) = uidForFlow.invoke(flow)
}

private class CountingTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}
