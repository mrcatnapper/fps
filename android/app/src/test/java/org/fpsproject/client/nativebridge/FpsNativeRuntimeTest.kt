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
    private val profileJsonWithShaper = """
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
          "codec": {"max_frame_payload": 1280, "max_frame_padding": 64},
          "tun": {"enabled": true, "mtu": 1280},
          "shaper": {
            "enabled": true,
            "profile_id": "android-shaper-test",
            "record_size_cdf_c2s": [[512, 0.5], [1500, 1.0]],
            "record_size_cdf_s2c": [[640, 0.25], [1510, 1.0]],
            "inter_record_delay_us_cdf_c2s": [[1000, 0.7], [10000, 1.0]],
            "inter_record_delay_us_cdf_s2c": [[2000, 0.5], [15000, 1.0]],
            "covert_ratio_max": 0.25,
            "burst_records_max": 2,
            "jitter_ms": {"min": 1, "max": 3},
            "adaptive": {
              "enabled": false,
              "min_records": 4,
              "min_observation_ms": 500,
              "decay": 0.75,
              "snapshot_interval_ms": 1000
            },
            "deterministic_seed": 42
          }
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
        assertTrue(snapshot.carrierAuthConfigured)
        assertEquals(null, snapshot.tunFdOwnership)
        assertEquals(1L, backend.createdProfiles.single().first)
        assertEquals(profileJson, backend.createdProfiles.single().second)
        assertEquals(listOf(RuntimeAuthConfigCall(1L, "android-test-v5", uuid, key, 2000, 666, 16 * 1024, 2048)), backend.configuredAuth)
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
    fun createClosesNativeHandleWhenAuthConfigurationFails() {
        val backend = FakeNativeBackend(authConfigureError = "invalid_client_auth_config")

        val error = assertThrows(IllegalArgumentException::class.java) {
            FpsNativeRuntime.create(profileJson, backend)
        }

        assertEquals("invalid_client_auth_config", error.message)
        assertEquals(1, backend.createdProfiles.size)
        assertEquals(listOf(RuntimeAuthConfigCall(1L, "android-test-v5", uuid, key, 2000, 666, 16 * 1024, 2048)), backend.configuredAuth)
        assertEquals(listOf(1L), backend.closedHandles)
    }

    @Test
    fun createConfiguresInlineShaperProfile() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJsonWithShaper, backend)
        val snapshot = runtime.snapshot()

        assertTrue(snapshot.shaperConfigured)
        assertEquals("android-shaper-test", snapshot.shaperProfileId)
        assertEquals(1, backend.configuredShapers.size)
        val configured = backend.configuredShapers.single()
        assertEquals("android-shaper-test", configured.profileId)
        assertEquals(listOf(512L, 1500L), configured.recordSizeC2sLe)
        assertEquals(listOf(0.5, 1.0), configured.recordSizeC2sP)
        assertEquals(listOf(640L, 1510L), configured.recordSizeS2cLe)
        assertEquals(listOf(0.25, 1.0), configured.recordSizeS2cP)
        assertEquals(listOf(1000L, 10000L), configured.delayC2sLe)
        assertEquals(listOf(0.7, 1.0), configured.delayC2sP)
        assertEquals(listOf(2000L, 15000L), configured.delayS2cLe)
        assertEquals(listOf(0.5, 1.0), configured.delayS2cP)
        assertEquals(0.25, configured.covertRatioMax, 0.0)
        assertEquals(2, configured.burstRecordsMax)
        assertEquals(1L, configured.jitterMinMs)
        assertEquals(3L, configured.jitterMaxMs)
        assertFalse(configured.adaptiveEnabled)
        assertEquals(4, configured.adaptiveMinRecords)
        assertEquals(500L, configured.adaptiveMinObservationMs)
        assertEquals(0.75, configured.adaptiveDecay, 0.0)
        assertEquals(1000L, configured.adaptiveSnapshotIntervalMs)
        assertTrue(configured.deterministicSeedPresent)
        assertEquals(42L, configured.deterministicSeed)
        assertFalse(snapshot.toString().contains(uuid))
        assertFalse(snapshot.toString().contains(key))
    }

    @Test
    fun createClosesNativeHandleWhenShaperConfigurationFails() {
        val backend = FakeNativeBackend(shaperConfigureError = "invalid_shaper_profile")

        val error = assertThrows(IllegalArgumentException::class.java) {
            FpsNativeRuntime.create(profileJsonWithShaper, backend)
        }

        assertEquals("invalid_shaper_profile", error.message)
        assertEquals(1, backend.configuredAuth.size)
        assertEquals(1, backend.configuredShapers.size)
        assertEquals(listOf(1L), backend.closedHandles)
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
        val stoppedBeforePrepare = runtime.stopRawCarrier()
        val prepared = runtime.prepareRawCarrierSocket("127.0.0.1", 9443)
        val stoppedAfterPrepare = runtime.stopRawCarrier()
        val notPrepared = runtime.completeRawCarrierProtection(protectAllowed = true)
        val preparedAgain = runtime.prepareRawCarrierSocket("127.0.0.1", 9443)
        val connected = runtime.completeRawCarrierProtection(protectAllowed = true)
        val stoppedAfterConnect = runtime.stopRawCarrier()
        val stoppedAgain = runtime.stopRawCarrier()

        assertEquals(listOf(Triple(1L, "127.0.0.1", 9443), Triple(1L, "127.0.0.1", 9443)), backend.preparedRawCarriers)
        assertEquals(listOf(1L to true, 1L to true), backend.completedRawCarrierProtection)
        assertEquals(listOf(1L, 1L, 1L, 1L), backend.stoppedRawCarriers)
        assertEquals(-1, stoppedBeforePrepare.rawCarrierProtectFd)
        assertEquals(77, prepared.rawCarrierProtectFd)
        assertEquals(-1, stoppedAfterPrepare.rawCarrierProtectFd)
        assertFalse(stoppedAfterPrepare.rawCarrierActive)
        assertEquals("raw_carrier_not_prepared", notPrepared.lastError)
        assertEquals(77, preparedAgain.rawCarrierProtectFd)
        assertTrue(connected.rawCarrierActive)
        assertEquals(1L, connected.rawCarrierConnectAttempted)
        assertEquals(1L, connected.rawCarrierConnectSucceeded)
        assertFalse(stoppedAfterConnect.rawCarrierActive)
        assertEquals(-1, stoppedAfterConnect.rawCarrierProtectFd)
        assertFalse(stoppedAgain.rawCarrierActive)
        assertEquals(-1, stoppedAgain.rawCarrierProtectFd)
    }

    @Test
    fun rawCarrierBridgeRequiresConnectedCarrierAndDelegatesToNativeBackend() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val stoppedRuntime = runtime.startRawCarrierBridge()
        runtime.start()
        val notConnected = runtime.startRawCarrierBridge()
        runtime.prepareRawCarrierSocket("127.0.0.1", 9443)
        runtime.completeRawCarrierProtection(protectAllowed = true)
        val listening = runtime.startRawCarrierBridge()
        val stopped = runtime.stopRawCarrier()

        assertEquals(listOf(1L, 1L, 1L), backend.startedRawCarrierBridges)
        assertEquals("runtime_stopped", stoppedRuntime.lastError)
        assertFalse(stoppedRuntime.rawCarrierBridgeListening)
        assertEquals("raw_carrier_not_connected", notConnected.lastError)
        assertFalse(notConnected.rawCarrierBridgeListening)
        assertTrue(listening.rawCarrierBridgeListening)
        assertEquals(18080, listening.rawCarrierBridgeListenPort)
        assertFalse(listening.rawCarrierBridgeActive)
        assertFalse(stopped.rawCarrierBridgeListening)
        assertEquals(0, stopped.rawCarrierBridgeListenPort)
        assertFalse(stopped.rawCarrierBridgeActive)
    }

    @Test
    fun rawCarrierAfterCloseReportsClosedRuntime() {
        val runtime = FpsNativeRuntime.create(profileJson, FakeNativeBackend())

        runtime.close()
        val snapshot = runtime.prepareRawCarrierSocket("127.0.0.1", 9443)

        assertFalse(snapshot.alive)
        assertEquals("runtime_closed", snapshot.lastError)
    }

    @Test
    fun clientAuthSmokeRequiresStartedRuntimeAndEmitsLeaseEvent() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val stopped = runtime.runClientAuthSmokeForTest()
        runtime.start()
        val succeeded = runtime.runClientAuthSmokeForTest()
        val events = runtime.drainNativeEvents(8)

        assertEquals("runtime_stopped", stopped.lastError)
        assertEquals(1L, stopped.carrierAuthFailed)
        assertEquals(1L, stopped.carrierAuthAttempted)
        assertEquals(null, succeeded.lastError)
        assertEquals(2L, succeeded.carrierAuthAttempted)
        assertEquals(1L, succeeded.carrierAuthSucceeded)
        assertEquals(1L, succeeded.carrierAuthFailed)
        assertEquals(1L, succeeded.carrierLeaseReceived)
        assertEquals(listOf(false, false), backend.authSmokeTamperFlags)
        assertEquals(1, events.size)
        assertEquals(NATIVE_EVENT_LEASE_RECEIVED, events.single().type)
        assertEquals(0x0a420002L, events.single().clientIpv4)
        assertEquals(0x0a420001L, events.single().serverIpv4)
        assertEquals(30, events.single().prefixLength)
        assertEquals(1280, events.single().mtu)
        assertEquals(leaseEvent().tunLeaseOrNull(), events.single().tunLeaseOrNull())
    }

    @Test
    fun clientAuthSmokeFailureEmitsFailureEventWithoutLease() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        runtime.start()
        val failed = runtime.runClientAuthSmokeForTest(tamperServerAccept = true)
        val events = runtime.drainNativeEvents(8)

        assertEquals("carrier_auth_failed", failed.lastError)
        assertEquals(1L, failed.carrierAuthAttempted)
        assertEquals(0L, failed.carrierAuthSucceeded)
        assertEquals(1L, failed.carrierAuthFailed)
        assertEquals(0L, failed.carrierLeaseReceived)
        assertEquals(1, events.size)
        assertEquals(NATIVE_EVENT_CARRIER_AUTH_FAILED, events.single().type)
        assertEquals("carrier_auth_failed", events.single().error)
        assertEquals(null, events.single().tunLeaseOrNull())
    }
}

private data class RuntimeAuthConfigCall(
    val handle: Long,
    val profileId: String,
    val clientUuid: String,
    val serverPublicKeyBase64: String,
    val clientUpgradeDelayMs: Long,
    val clientUpgradeDelaySigmaMs: Long,
    val maxFramePayload: Int,
    val maxFramePadding: Int,
)

private data class RuntimeShaperConfigCall(
    val handle: Long,
    val profileId: String,
    val recordSizeC2sLe: List<Long>,
    val recordSizeC2sP: List<Double>,
    val recordSizeS2cLe: List<Long>,
    val recordSizeS2cP: List<Double>,
    val delayC2sLe: List<Long>,
    val delayC2sP: List<Double>,
    val delayS2cLe: List<Long>,
    val delayS2cP: List<Double>,
    val covertRatioMax: Double,
    val burstRecordsMax: Int,
    val jitterMinMs: Long,
    val jitterMaxMs: Long,
    val adaptiveEnabled: Boolean,
    val adaptiveMinRecords: Int,
    val adaptiveMinObservationMs: Long,
    val adaptiveDecay: Double,
    val adaptiveSnapshotIntervalMs: Long,
    val deterministicSeedPresent: Boolean,
    val deterministicSeed: Long,
)

private fun leaseEvent() = NativeRuntimeEvent(
    type = NATIVE_EVENT_LEASE_RECEIVED,
    clientIpv4 = 0x0a420002L,
    serverIpv4 = 0x0a420001L,
    prefixLength = 30,
    mtu = 1280,
)

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
    tunPacketsWritten: Long = 0,
    tunBytesWritten: Long = 0,
    tunInboundWriteRejected: Long = 0,
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
    rawCarrierBridgeListening: Boolean = false,
    rawCarrierBridgeListenPort: Int = 0,
    rawCarrierBridgeActive: Boolean = false,
    rawCarrierConnectAttempted: Long = 0,
    rawCarrierConnectSucceeded: Long = 0,
    rawCarrierConnectFailed: Long = 0,
    carrierAuthConfigured: Boolean = false,
    carrierAuthAttempted: Long = 0,
    carrierAuthSucceeded: Long = 0,
    carrierAuthFailed: Long = 0,
    carrierLeaseReceived: Long = 0,
    shaperConfigured: Boolean = false,
    shaperProfileId: String? = null,
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
    tunPacketsWritten = tunPacketsWritten,
    tunBytesWritten = tunBytesWritten,
    tunInboundWriteRejected = tunInboundWriteRejected,
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
    rawCarrierBridgeListening = rawCarrierBridgeListening,
    rawCarrierBridgeListenPort = rawCarrierBridgeListenPort,
    rawCarrierBridgeActive = rawCarrierBridgeActive,
    rawCarrierConnectAttempted = rawCarrierConnectAttempted,
    rawCarrierConnectSucceeded = rawCarrierConnectSucceeded,
    rawCarrierConnectFailed = rawCarrierConnectFailed,
    carrierAuthConfigured = carrierAuthConfigured,
    carrierAuthAttempted = carrierAuthAttempted,
    carrierAuthSucceeded = carrierAuthSucceeded,
    carrierAuthFailed = carrierAuthFailed,
    carrierLeaseReceived = carrierLeaseReceived,
    shaperConfigured = shaperConfigured,
    shaperProfileId = shaperProfileId,
    lastError = lastError,
)

private class FakeNativeBackend(
    private val returnZeroHandle: Boolean = false,
    private val authConfigureError: String? = null,
    private val shaperConfigureError: String? = null,
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
    val startedRawCarrierBridges = mutableListOf<Long>()
    val stoppedRawCarriers = mutableListOf<Long>()
    val configuredAuth = mutableListOf<RuntimeAuthConfigCall>()
    val configuredShapers = mutableListOf<RuntimeShaperConfigCall>()
    val authSmokeTamperFlags = mutableListOf<Boolean>()
    var drainCalls = 0
    private val snapshots = mutableMapOf<Long, NativeRuntimeSnapshot>()
    private val pendingPolicy = ArrayDeque(initialPolicyPackets)
    private val inFlightPolicy = mutableSetOf<Long>()
    private val nativeEvents = ArrayDeque<NativeRuntimeEvent>()

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

    override fun startRawCarrierBridge(handle: Long): NativeRuntimeSnapshot {
        startedRawCarrierBridges += handle
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            return current.copy(lastError = "runtime_stopped").also { snapshots[handle] = it }
        }
        if (!current.rawCarrierActive) {
            return current.copy(
                rawCarrierBridgeListening = false,
                rawCarrierBridgeListenPort = 0,
                rawCarrierBridgeActive = false,
                lastError = "raw_carrier_not_connected",
            ).also { snapshots[handle] = it }
        }
        return current.copy(
            rawCarrierBridgeListening = true,
            rawCarrierBridgeListenPort = 18080,
            rawCarrierBridgeActive = false,
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
            rawCarrierBridgeListening = false,
            rawCarrierBridgeListenPort = 0,
            rawCarrierBridgeActive = false,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun configureClientAuth(
        handle: Long,
        profileId: String,
        clientUuid: String,
        serverPublicKeyBase64: String,
        clientUpgradeDelayMs: Long,
        clientUpgradeDelaySigmaMs: Long,
        maxFramePayload: Int,
        maxFramePadding: Int,
    ): NativeRuntimeSnapshot {
        configuredAuth += RuntimeAuthConfigCall(
            handle,
            profileId,
            clientUuid,
            serverPublicKeyBase64,
            clientUpgradeDelayMs,
            clientUpgradeDelaySigmaMs,
            maxFramePayload,
            maxFramePadding,
        )
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        if (authConfigureError != null) {
            return current.copy(
                carrierAuthConfigured = false,
                lastError = authConfigureError,
            ).also { snapshots[handle] = it }
        }
        return current.copy(
            carrierAuthConfigured = true,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun configureClientShaper(
        handle: Long,
        profileId: String,
        recordSizeC2sLe: LongArray,
        recordSizeC2sP: DoubleArray,
        recordSizeS2cLe: LongArray,
        recordSizeS2cP: DoubleArray,
        delayC2sLe: LongArray,
        delayC2sP: DoubleArray,
        delayS2cLe: LongArray,
        delayS2cP: DoubleArray,
        covertRatioMax: Double,
        burstRecordsMax: Int,
        jitterMinMs: Long,
        jitterMaxMs: Long,
        adaptiveEnabled: Boolean,
        adaptiveMinRecords: Int,
        adaptiveMinObservationMs: Long,
        adaptiveDecay: Double,
        adaptiveSnapshotIntervalMs: Long,
        deterministicSeedPresent: Boolean,
        deterministicSeed: Long,
    ): NativeRuntimeSnapshot {
        configuredShapers += RuntimeShaperConfigCall(
            handle = handle,
            profileId = profileId,
            recordSizeC2sLe = recordSizeC2sLe.toList(),
            recordSizeC2sP = recordSizeC2sP.toList(),
            recordSizeS2cLe = recordSizeS2cLe.toList(),
            recordSizeS2cP = recordSizeS2cP.toList(),
            delayC2sLe = delayC2sLe.toList(),
            delayC2sP = delayC2sP.toList(),
            delayS2cLe = delayS2cLe.toList(),
            delayS2cP = delayS2cP.toList(),
            covertRatioMax = covertRatioMax,
            burstRecordsMax = burstRecordsMax,
            jitterMinMs = jitterMinMs,
            jitterMaxMs = jitterMaxMs,
            adaptiveEnabled = adaptiveEnabled,
            adaptiveMinRecords = adaptiveMinRecords,
            adaptiveMinObservationMs = adaptiveMinObservationMs,
            adaptiveDecay = adaptiveDecay,
            adaptiveSnapshotIntervalMs = adaptiveSnapshotIntervalMs,
            deterministicSeedPresent = deterministicSeedPresent,
            deterministicSeed = deterministicSeed,
        )
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        if (shaperConfigureError != null) {
            return current.copy(
                shaperConfigured = false,
                shaperProfileId = null,
                lastError = shaperConfigureError,
            ).also { snapshots[handle] = it }
        }
        return current.copy(
            shaperConfigured = true,
            shaperProfileId = profileId,
            lastError = null,
        ).also { snapshots[handle] = it }
    }

    override fun runClientAuthSmokeForTest(handle: Long, tamperServerAccept: Boolean): NativeRuntimeSnapshot {
        authSmokeTamperFlags += tamperServerAccept
        val current = snapshots[handle] ?: return nativeSnapshot(alive = false, lastError = "invalid_handle")
        if (!current.started) {
            val snapshot = current.copy(
                carrierAuthAttempted = current.carrierAuthAttempted + 1,
                carrierAuthFailed = current.carrierAuthFailed + 1,
                lastError = "runtime_stopped",
            )
            snapshots[handle] = snapshot
            return snapshot
        }
        if (!current.carrierAuthConfigured) {
            val snapshot = current.copy(
                carrierAuthAttempted = current.carrierAuthAttempted + 1,
                carrierAuthFailed = current.carrierAuthFailed + 1,
                lastError = "client_auth_not_configured",
            )
            snapshots[handle] = snapshot
            return snapshot
        }
        if (tamperServerAccept) {
            nativeEvents += NativeRuntimeEvent(type = NATIVE_EVENT_CARRIER_AUTH_FAILED, error = "carrier_auth_failed")
            val snapshot = current.copy(
                carrierAuthAttempted = current.carrierAuthAttempted + 1,
                carrierAuthFailed = current.carrierAuthFailed + 1,
                lastError = "carrier_auth_failed",
            )
            snapshots[handle] = snapshot
            return snapshot
        }
        nativeEvents += leaseEvent()
        val snapshot = current.copy(
            carrierAuthAttempted = current.carrierAuthAttempted + 1,
            carrierAuthSucceeded = current.carrierAuthSucceeded + 1,
            carrierLeaseReceived = current.carrierLeaseReceived + 1,
            lastError = null,
        )
        snapshots[handle] = snapshot
        return snapshot
    }

    override fun drainNativeEvents(handle: Long, maxEvents: Int): List<NativeRuntimeEvent> {
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

private class FakeTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}
