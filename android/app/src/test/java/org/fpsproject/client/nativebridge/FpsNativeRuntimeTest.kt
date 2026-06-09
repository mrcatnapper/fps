package org.fpsproject.client.nativebridge

import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.TunHandle
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.TunProtocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Base64

class FpsNativeRuntimeTest {
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
          "tun": {"enabled": true, "mtu": 1280}
        }
    """.trimIndent()

    @Test
    fun createValidatesProfileBeforeNativeHandleCreation() {
        val backend = FakeNativeBackend()

        assertThrows(IllegalArgumentException::class.java) {
            FpsNativeRuntime.create("""{"network": {"server": "fps.example.test:443"}}""", backend)
        }

        assertEquals(0, backend.createdProfiles.size)
    }

    @Test
    fun createSnapshotAndCloseUseOpaqueHandle() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val snapshot = runtime.snapshot()

        assertTrue(snapshot.alive)
        assertFalse(snapshot.started)
        assertFalse(snapshot.workerThreadRunning)
        assertFalse(snapshot.tunAttached)
        assertFalse(snapshot.tunPumpRunning)
        assertEquals(null, snapshot.tunFdOwnership)
        assertEquals(1L, backend.createdProfiles.single().first)
        assertEquals(profileJson, backend.createdProfiles.single().second)
        assertFalse(snapshot.toString().contains(profileJson))
        assertFalse(snapshot.toString().contains(uuid))
        assertFalse(snapshot.toString().contains(key))

        runtime.close()
        runtime.close()

        assertEquals(listOf(1L), backend.closedHandles)
        assertEquals("runtime_closed", runtime.snapshot().lastError)
    }

    @Test
    fun createRejectsZeroNativeHandle() {
        val backend = FakeNativeBackend(returnZeroHandle = true)

        val error = assertThrows(IllegalArgumentException::class.java) {
            FpsNativeRuntime.create(profileJson, backend)
        }

        assertEquals("native runtime creation failed", error.message)
        assertEquals(1, backend.createdProfiles.size)
        assertEquals(emptyList<Long>(), backend.closedHandles)
    }

    @Test
    fun startStopAndNoopCommandsUseExecutorLifecycle() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val started = runtime.start()
        val startedAgain = runtime.start()
        val posted = runtime.postNoopCommand()
        val stopped = runtime.stop()
        val stoppedAgain = runtime.stop()
        val rejected = runtime.postNoopCommand()

        assertTrue(started.started)
        assertTrue(started.workerThreadRunning)
        assertTrue(startedAgain.started)
        assertEquals(1L, posted.commandsPosted)
        assertEquals(1L, posted.commandsCompleted)
        assertFalse(stopped.started)
        assertFalse(stopped.workerThreadRunning)
        assertFalse(stoppedAgain.started)
        assertEquals("runtime_stopped", rejected.lastError)
        assertEquals(1L, rejected.commandsPosted)
        assertEquals(1L, rejected.commandsCompleted)
    }

    @Test
    fun tunPumpRequiresStartedRuntimeAndAttachedTun() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val stopped = runtime.startTunPump()
        runtime.start()
        val missingTun = runtime.startTunPump()

        assertFalse(stopped.tunPumpRunning)
        assertEquals("runtime_stopped", stopped.lastError)
        assertFalse(missingTun.tunPumpRunning)
        assertEquals("tun_not_attached", missingTun.lastError)
    }

    @Test
    fun tunPumpStartStopIsIdempotent() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        runtime.start()
        runtime.attachTunFdOwnedDuplicate(77, 1280)
        val started = runtime.startTunPump()
        val startedAgain = runtime.startTunPump()
        val stopped = runtime.stopTunPump()
        val stoppedAgain = runtime.stopTunPump()

        assertTrue(started.tunPumpRunning)
        assertTrue(startedAgain.tunPumpRunning)
        assertFalse(stopped.tunPumpRunning)
        assertFalse(stoppedAgain.tunPumpRunning)
        assertEquals(listOf(1L, 1L), backend.startedTunPumps)
        assertEquals(listOf(1L, 1L), backend.stoppedTunPumps)
    }

    @Test
    fun attachTunDuplicatesFdWithoutOwningKotlinHandle() {
        val backend = FakeNativeBackend()
        val handle = FakeTunHandle(fd = 77)
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val snapshot = runtime.attachTun(EstablishedTun.owned(fd = handle.fd, mtu = 1280, handle = handle))
        runtime.close()

        assertTrue(snapshot.tunAttached)
        assertEquals(77, snapshot.tunFd)
        assertEquals(1280, snapshot.tunMtu)
        assertEquals(TUN_FD_OWNERSHIP_OWNED_DUPLICATE, snapshot.tunFdOwnership)
        assertEquals(0, handle.closeCount)
        assertEquals(listOf(Triple(1L, 77, 1280)), backend.attachedTun)
    }

    @Test
    fun attachTunRejectsInvalidFdAndMtu() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val badFd = runtime.attachTunFdOwnedDuplicate(-1, 1280)
        val badMtu = runtime.attachTunFdOwnedDuplicate(77, 0)

        assertFalse(badFd.tunAttached)
        assertEquals(null, badFd.tunFdOwnership)
        assertEquals("invalid_tun_fd", badFd.lastError)
        assertFalse(badMtu.tunAttached)
        assertEquals(null, badMtu.tunFdOwnership)
        assertEquals("invalid_tun_mtu", badMtu.lastError)
    }

    @Test
    fun attachAfterCloseReportsClosedRuntime() {
        val runtime = FpsNativeRuntime.create(profileJson, FakeNativeBackend())

        runtime.close()
        val snapshot = runtime.attachTunFdOwnedDuplicate(77, 1280)

        assertFalse(snapshot.alive)
        assertEquals("runtime_closed", snapshot.lastError)
    }

    @Test
    fun tunPolicyPacketsDrainAsMetadataAndCompleteByOpaquePacketId() {
        val backend = FakeNativeBackend(
            initialPolicyPackets = listOf(
                NativeTunPolicyPacket(
                    packetId = 41,
                    packetSize = 28,
                    flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x5db8d822, 443),
                ),
                NativeTunPolicyPacket(
                    packetId = 42,
                    packetSize = 40,
                    flow = TunFlowTuple(TunProtocol.TCP, 0x0a420002, 53001, 0x5db8d822, 8443),
                ),
            ),
        )
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val drained = runtime.drainTunPolicyPackets(maxPackets = 1)
        val afterAllow = runtime.completeTunPolicyPacket(41, SplitTunnelDecision.ALLOW)
        val next = runtime.drainTunPolicyPackets(maxPackets = 8)
        val afterDrop = runtime.completeTunPolicyPacket(42, SplitTunnelDecision.DROP)

        assertEquals(1, drained.size)
        assertEquals(41L, drained.single().packetId)
        assertEquals(28, drained.single().packetSize)
        assertEquals(TunProtocol.UDP, drained.single().flow.protocol)
        assertEquals(1L, afterAllow.tunPolicyAllowed)
        assertEquals(42L, next.single().packetId)
        assertEquals(1L, afterDrop.tunPolicyAllowed)
        assertEquals(1L, afterDrop.tunPolicyDropped)
        assertEquals(0L, afterDrop.tunPolicyPending)
        assertEquals(0L, afterDrop.tunPolicyInFlight)
        assertEquals(listOf(41L to SplitTunnelDecision.ALLOW, 42L to SplitTunnelDecision.DROP), backend.completedPolicy)
    }

    @Test
    fun tunPolicyDrainRejectsNonPositiveLimitsBeforeCallingNativeBackend() {
        val backend = FakeNativeBackend(
            initialPolicyPackets = listOf(
                NativeTunPolicyPacket(
                    packetId = 41,
                    packetSize = 28,
                    flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x5db8d822, 443),
                ),
            ),
        )
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        assertEquals(emptyList<NativeTunPolicyPacket>(), runtime.drainTunPolicyPackets(maxPackets = 0))
        assertEquals(emptyList<NativeTunPolicyPacket>(), runtime.drainTunPolicyPackets(maxPackets = -1))
        assertEquals(0, backend.drainCalls)
        assertEquals(1, runtime.drainTunPolicyPackets(maxPackets = 1).size)
    }

    @Test
    fun tunPolicyCompletionRejectsUnknownPacketId() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val snapshot = runtime.completeTunPolicyPacket(999, SplitTunnelDecision.ALLOW)

        assertEquals("unknown_tun_policy_packet_id", snapshot.lastError)
        assertEquals(0L, snapshot.tunPolicyAllowed)
        assertEquals(0L, snapshot.tunPolicyDropped)
    }

    @Test
    fun allowedTunPolicyPacketRequiresOutboundTransport() {
        val backend = FakeNativeBackend(
            initialPolicyPackets = listOf(
                NativeTunPolicyPacket(
                    packetId = 41,
                    packetSize = 28,
                    flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x5db8d822, 443),
                ),
            ),
        )
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val packet = runtime.drainTunPolicyPackets(maxPackets = 1).single()
        val snapshot = runtime.completeTunPolicyPacket(packet.packetId, SplitTunnelDecision.ALLOW)

        assertEquals("no_carrier_transport", snapshot.lastError)
        assertEquals("no_carrier_transport", snapshot.tunLastDropReason)
        assertEquals(1L, snapshot.tunPolicyAllowed)
        assertEquals(0L, snapshot.tunPolicyDropped)
        assertEquals(1L, snapshot.tunPacketsDropped)
        assertEquals(1L, snapshot.tunCovertEnqueueAttempted)
        assertEquals(0L, snapshot.tunCovertEnqueueAccepted)
        assertEquals(1L, snapshot.tunCovertEnqueueRejected)
    }

    @Test
    fun droppedTunPolicyPacketDoesNotAttemptOutboundTransport() {
        val backend = FakeNativeBackend(
            initialPolicyPackets = listOf(
                NativeTunPolicyPacket(
                    packetId = 41,
                    packetSize = 28,
                    flow = TunFlowTuple(TunProtocol.UDP, 0x0a420002, 53000, 0x5db8d822, 443),
                ),
            ),
        )
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val packet = runtime.drainTunPolicyPackets(maxPackets = 1).single()
        val snapshot = runtime.completeTunPolicyPacket(packet.packetId, SplitTunnelDecision.DROP)

        assertEquals(null, snapshot.lastError)
        assertEquals(0L, snapshot.tunCovertEnqueueAttempted)
        assertEquals(0L, snapshot.tunCovertEnqueueAccepted)
        assertEquals(0L, snapshot.tunCovertEnqueueRejected)
        assertEquals(0L, snapshot.tunPolicyAllowed)
        assertEquals(1L, snapshot.tunPolicyDropped)
        assertEquals(1L, snapshot.tunPacketsDropped)
    }

    @Test
    fun rawCarrierLifecycleDelegatesToNativeBackend() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        runtime.start()
        val prepared = runtime.prepareRawCarrierSocket("127.0.0.1", 9443)
        val connected = runtime.completeRawCarrierProtection(protectAllowed = true)
        val stopped = runtime.stopRawCarrier()

        assertEquals(listOf(Triple(1L, "127.0.0.1", 9443)), backend.preparedRawCarriers)
        assertEquals(listOf(1L to true), backend.completedRawCarrierProtection)
        assertEquals(listOf(1L), backend.stoppedRawCarriers)
        assertEquals(77, prepared.rawCarrierProtectFd)
        assertTrue(connected.rawCarrierActive)
        assertEquals(1L, connected.rawCarrierConnectAttempted)
        assertEquals(1L, connected.rawCarrierConnectSucceeded)
        assertFalse(stopped.rawCarrierActive)
        assertEquals(-1, stopped.rawCarrierProtectFd)
    }

    @Test
    fun rawCarrierAfterCloseReportsClosedRuntime() {
        val runtime = FpsNativeRuntime.create(profileJson, FakeNativeBackend())

        runtime.close()
        val snapshot = runtime.prepareRawCarrierSocket("127.0.0.1", 9443)

        assertFalse(snapshot.alive)
        assertEquals("runtime_closed", snapshot.lastError)
    }
}

private fun nativeSnapshot(
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
    lastError = lastError,
)

private class FakeNativeBackend(
    private val returnZeroHandle: Boolean = false,
    initialPolicyPackets: List<NativeTunPolicyPacket> = emptyList(),
) : FpsNativeBackend {
    private var nextHandle = 1L
    val createdProfiles = mutableListOf<Pair<Long, String>>()
    val closedHandles = mutableListOf<Long>()
    val attachedTun = mutableListOf<Triple<Long, Int, Int>>()
    val startedTunPumps = mutableListOf<Long>()
    val stoppedTunPumps = mutableListOf<Long>()
    val completedPolicy = mutableListOf<Pair<Long, SplitTunnelDecision>>()
    val preparedRawCarriers = mutableListOf<Triple<Long, String, Int>>()
    val completedRawCarrierProtection = mutableListOf<Pair<Long, Boolean>>()
    val stoppedRawCarriers = mutableListOf<Long>()
    var drainCalls = 0
    private val snapshots = mutableMapOf<Long, NativeRuntimeSnapshot>()
    private val pendingPolicy = ArrayDeque(initialPolicyPackets)
    private val inFlightPolicy = mutableSetOf<Long>()

    override fun createRuntime(profileText: String): Long {
        val handle = nextHandle++
        createdProfiles += handle to profileText
        if (returnZeroHandle) {
            return 0
        }
        snapshots[handle] = nativeSnapshot()
        return handle
    }

    override fun closeRuntime(handle: Long) {
        closedHandles += handle
        snapshots.remove(handle)
    }

    override fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot {
        return snapshots[handle] ?: nativeSnapshot(
            alive = false,
            lastError = "invalid_handle",
        )
    }

    override fun startTunPump(handle: Long): NativeRuntimeSnapshot {
        startedTunPumps += handle
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            return current.copy(tunPumpRunning = false, lastError = "runtime_stopped").also { snapshots[handle] = it }
        }
        if (!current.tunAttached) {
            return current.copy(tunPumpRunning = false, lastError = "tun_not_attached").also { snapshots[handle] = it }
        }
        return current.copy(
            tunPumpRunning = true,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun stopTunPump(handle: Long): NativeRuntimeSnapshot {
        stoppedTunPumps += handle
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            tunPumpRunning = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun startRuntime(handle: Long): NativeRuntimeSnapshot {
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            started = true,
            workerThreadRunning = true,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun stopRuntime(handle: Long): NativeRuntimeSnapshot {
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            started = false,
            workerThreadRunning = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun postNoopCommand(handle: Long): NativeRuntimeSnapshot {
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
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
        snapshots[handle] ?: return nativeSnapshot(
            alive = false,
            lastError = "invalid_handle",
        )
        if (fd < 0) {
            return nativeSnapshot(
                tunAttached = false,
                tunPumpRunning = false,
                lastError = "invalid_tun_fd",
            ).also { snapshots[handle] = it }
        }
        if (mtu <= 0) {
            return nativeSnapshot(
                tunAttached = false,
                tunPumpRunning = false,
                lastError = "invalid_tun_mtu",
            ).also { snapshots[handle] = it }
        }
        val current = snapshots[handle] ?: nativeSnapshot()
        val snapshot = current.copy(
            tunAttached = true,
            tunFd = fd,
            tunMtu = mtu,
            tunFdOwnership = TUN_FD_OWNERSHIP_OWNED_DUPLICATE,
            lastError = null,
        )
        snapshots[handle] = snapshot
        return snapshot
    }

    override fun drainTunPolicyPackets(handle: Long, maxPackets: Int): List<NativeTunPolicyPacket> {
        drainCalls += 1
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
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
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
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
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
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
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
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        return current.copy(
            rawCarrierProtectFd = -1,
            rawCarrierConnecting = false,
            rawCarrierActive = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

}

private class FakeTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}
