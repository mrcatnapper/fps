package org.fpsproject.client.nativebridge

import android.os.ParcelFileDescriptor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.TunProtocol
import java.io.FileOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.security.MessageDigest
import java.util.Base64
import kotlin.concurrent.thread

class NativeCoreSmokeInstrumentedTest {
    private val profileJson = """
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
          "tun": {"enabled": true, "mtu": 1280}
        }
    """.trimIndent()

    @Test
    fun loadsNativeLibraryAndRunsCoreSmoke() {
        assertTrue(FpsNative.nativeVersion().startsWith("fps-android-native/"))
        assertEquals("ok", FpsNative.nativeCoreSmoke())
    }

    @Test
    fun parsesIpv4TcpFlowTuple() {
        val packet = byteArrayOf(
            0x45, 0x00, 0x00, 0x28,
            0x00, 0x00, 0x40, 0x00,
            0x40, 0x06, 0x00, 0x00,
            0x0a, 0x42, 0x00, 0x02,
            0x5d.toByte(), 0xb8.toByte(), 0xd8.toByte(), 0x22,
            0xcf.toByte(), 0x08, 0x01, 0xbb.toByte(),
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x50, 0x02, 0x20, 0x00,
            0x00, 0x00, 0x00, 0x00,
        )

        val tuple = FpsNative.parseIpv4FlowTuple(packet)!!

        assertEquals(TunProtocol.TCP, tuple.protocol)
        assertEquals(0x0a420002L, tuple.sourceIpv4)
        assertEquals(53000, tuple.sourcePort)
        assertEquals(0x5db8d822L, tuple.destinationIpv4)
        assertEquals(443, tuple.destinationPort)
    }

    @Test
    fun nativeRuntimeHandleLifecycleAndOwnedDuplicateTunSnapshot() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        val initial = FpsNative.runtimeSnapshot(handle)
        assertTrue(initial.alive)
        assertFalse(initial.started)
        assertFalse(initial.workerThreadRunning)
        assertFalse(initial.tunAttached)
        assertEquals(null, initial.tunFdOwnership)
        assertEquals(null, initial.lastError)

        val started = FpsNative.startRuntime(handle)
        assertTrue(started.started)
        assertTrue(started.workerThreadRunning)
        assertEquals(0L, started.commandsPosted)
        assertEquals(0L, started.commandsCompleted)

        val posted = FpsNative.postNoopCommand(handle)
        assertEquals(1L, posted.commandsPosted)
        assertEquals(1L, posted.commandsCompleted)

        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        val attached = FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        assertTrue(attached.alive)
        assertTrue(attached.tunAttached)
        assertNotEquals(readEnd.fd, attached.tunFd)
        assertEquals(1280, attached.tunMtu)
        assertEquals(TUN_FD_OWNERSHIP_OWNED_DUPLICATE, attached.tunFdOwnership)

        val badFd = FpsNative.attachTunFdOwnedDuplicate(handle, -1, 1280)
        assertTrue(badFd.alive)
        assertFalse(badFd.tunAttached)
        assertEquals(null, badFd.tunFdOwnership)
        assertEquals("invalid_tun_fd", badFd.lastError)

        val badMtu = FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 0)
        assertTrue(badMtu.alive)
        assertFalse(badMtu.tunAttached)
        assertEquals(null, badMtu.tunFdOwnership)
        assertEquals("invalid_tun_mtu", badMtu.lastError)

        val reattached = FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        assertTrue(reattached.tunAttached)
        val pumpStarted = FpsNative.startTunPump(handle)
        assertTrue(pumpStarted.tunPumpRunning)

        val output = FileOutputStream(writeEnd.fileDescriptor)
        output.write(validUdpPacket())
        output.flush()
        val parsed = awaitSnapshot(handle) { it.tunPacketsParsed == 1L }
        assertEquals(1L, parsed.tunPacketsRead)
        assertEquals(28L, parsed.tunBytesRead)
        assertEquals(1L, parsed.tunPacketsParsed)
        assertEquals(0L, parsed.tunPacketsDropped)
        assertEquals(1L, parsed.tunPolicyPending)
        assertEquals(null, parsed.tunLastDropReason)

        val pendingAllow = FpsNative.drainTunPolicyPackets(handle, 8)
        assertEquals(1, pendingAllow.size)
        assertEquals(28, pendingAllow.single().packetSize)
        assertEquals(TunProtocol.UDP, pendingAllow.single().flow.protocol)
        assertEquals(0x0a420002L, pendingAllow.single().flow.sourceIpv4)
        val allowed = FpsNative.completeTunPolicyPacket(handle, pendingAllow.single().packetId, SplitTunnelDecision.ALLOW)
        assertEquals(0L, allowed.tunPolicyPending)
        assertEquals(0L, allowed.tunPolicyInFlight)
        assertEquals(1L, allowed.tunPolicyAllowed)
        assertEquals(0L, allowed.tunPolicyDropped)
        assertEquals(1L, allowed.tunPacketsDropped)
        assertEquals(1L, allowed.tunCovertEnqueueAttempted)
        assertEquals(0L, allowed.tunCovertEnqueueAccepted)
        assertEquals(1L, allowed.tunCovertEnqueueRejected)
        assertEquals("no_carrier_transport", allowed.tunLastDropReason)
        assertEquals("no_carrier_transport", allowed.lastError)

        output.write(validUdpPacket())
        output.flush()
        val parsedForDrop = awaitSnapshot(handle) { it.tunPacketsParsed == 2L && it.tunPolicyPending == 1L }
        assertEquals(2L, parsedForDrop.tunPacketsRead)
        val pendingDrop = FpsNative.drainTunPolicyPackets(handle, 8)
        assertEquals(1, pendingDrop.size)
        val policyDropped = FpsNative.completeTunPolicyPacket(handle, pendingDrop.single().packetId, SplitTunnelDecision.DROP)
        assertEquals(1L, policyDropped.tunPolicyAllowed)
        assertEquals(1L, policyDropped.tunPolicyDropped)
        assertEquals(2L, policyDropped.tunPacketsDropped)
        assertEquals("tun_policy_drop", policyDropped.tunLastDropReason)

        output.write(byteArrayOf(0x60))
        output.flush()
        val dropped = awaitSnapshot(handle) { it.tunPacketsDropped == 3L }
        assertEquals(3L, dropped.tunPacketsRead)
        assertEquals(57L, dropped.tunBytesRead)
        assertEquals(2L, dropped.tunPacketsParsed)
        assertEquals(3L, dropped.tunPacketsDropped)
        assertEquals("non_ipv4_packet", dropped.tunLastDropReason)

        val pumpStopped = FpsNative.stopTunPump(handle)
        assertFalse(pumpStopped.tunPumpRunning)

        val stopped = FpsNative.stopRuntime(handle)
        assertFalse(stopped.started)
        assertFalse(stopped.workerThreadRunning)
        val rejected = FpsNative.postNoopCommand(handle)
        assertEquals("runtime_stopped", rejected.lastError)
        assertEquals(1L, rejected.commandsPosted)
        assertEquals(1L, rejected.commandsCompleted)

        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
        val closed = FpsNative.runtimeSnapshot(handle)
        assertFalse(closed.alive)
        assertEquals(null, closed.tunFdOwnership)
        assertEquals("invalid_handle", closed.lastError)
    }

    @Test
    fun nativeTunPolicyBridgeRejectsUnknownAndBoundsQueue() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        FpsNative.startTunPump(handle)
        val output = FileOutputStream(writeEnd.fileDescriptor)

        val unknown = FpsNative.completeTunPolicyPacket(handle, 999, SplitTunnelDecision.ALLOW)
        assertEquals("unknown_tun_policy_packet_id", unknown.lastError)
        assertEquals(0L, unknown.tunPolicyAllowed)
        assertEquals(0L, unknown.tunPolicyDropped)

        val nonPositiveDrain = FpsNative.drainTunPolicyPackets(handle, 0)
        assertEquals(0, nonPositiveDrain.size)

        repeat(257) { index ->
            output.write(validUdpPacket())
            output.flush()
            awaitSnapshot(handle) { it.tunPacketsRead >= (index + 1).toLong() }
        }
        val saturated = FpsNative.runtimeSnapshot(handle)
        assertEquals(256L, saturated.tunPolicyPending)
        assertEquals(0L, saturated.tunPolicyInFlight)
        assertEquals(1L, saturated.tunPolicyQueueFull)
        assertEquals(1L, saturated.tunPacketsDropped)
        assertEquals("tun_policy_queue_full", saturated.tunLastDropReason)

        val drained = FpsNative.drainTunPolicyPackets(handle, 300)
        assertEquals(256, drained.size)
        val afterDrain = FpsNative.runtimeSnapshot(handle)
        assertEquals(0L, afterDrain.tunPolicyPending)
        assertEquals(256L, afterDrain.tunPolicyInFlight)

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
    }

    @Test
    fun nativeTunPolicyBridgeClearsPendingAndInflightPacketsOnTunReattach() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val firstPipe = ParcelFileDescriptor.createPipe()
        val firstReadEnd = firstPipe[0]
        val firstWriteEnd = firstPipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, firstReadEnd.fd, 1280)
        FpsNative.startTunPump(handle)

        val output = FileOutputStream(firstWriteEnd.fileDescriptor)
        output.write(validUdpPacket())
        output.flush()
        val pending = awaitSnapshot(handle) { it.tunPolicyPending == 1L }
        assertEquals(1L, pending.tunPolicyPending)

        val drained = FpsNative.drainTunPolicyPackets(handle, 1)
        assertEquals(1, drained.size)
        assertEquals(1L, FpsNative.runtimeSnapshot(handle).tunPolicyInFlight)

        val secondPipe = ParcelFileDescriptor.createPipe()
        val secondReadEnd = secondPipe[0]
        val secondWriteEnd = secondPipe[1]
        val reattached = FpsNative.attachTunFdOwnedDuplicate(handle, secondReadEnd.fd, 1280)
        assertTrue(reattached.tunAttached)
        assertEquals(0L, reattached.tunPolicyPending)
        assertEquals(0L, reattached.tunPolicyInFlight)

        val oldCompletion = FpsNative.completeTunPolicyPacket(handle, drained.single().packetId, SplitTunnelDecision.ALLOW)
        assertEquals("unknown_tun_policy_packet_id", oldCompletion.lastError)
        assertEquals(0L, oldCompletion.tunPolicyAllowed)

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        firstReadEnd.close()
        firstWriteEnd.close()
        secondReadEnd.close()
        secondWriteEnd.close()
    }

    @Test
    fun nativeTunPolicyAllowHandsExactPacketToCaptureSink() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        FpsNative.startTunPump(handle)
        FpsNativeTestHooks.installTunPacketCaptureSink(handle, rejectPackets = false)

        val packet = validUdpPacket()
        val output = FileOutputStream(writeEnd.fileDescriptor)
        output.write(packet)
        output.flush()
        awaitSnapshot(handle) { it.tunPolicyPending == 1L }
        val pending = FpsNative.drainTunPolicyPackets(handle, 1).single()
        val allowed = FpsNative.completeTunPolicyPacket(handle, pending.packetId, SplitTunnelDecision.ALLOW)
        val digests = FpsNativeTestHooks.capturedTunPacketDigests(handle)

        assertEquals(1L, allowed.tunPolicyAllowed)
        assertEquals(0L, allowed.tunPacketsDropped)
        assertEquals(1L, allowed.tunCovertEnqueueAttempted)
        assertEquals(1L, allowed.tunCovertEnqueueAccepted)
        assertEquals(0L, allowed.tunCovertEnqueueRejected)
        assertEquals(null, allowed.lastError)
        assertEquals(listOf(sha256Hex(packet)), digests.toList())

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
    }

    @Test
    fun nativeFakeCarrierLifecycleRequiresStartedRuntimeAndIsIdempotent() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        val stoppedRuntime = FpsNativeTestHooks.startFakeCarrier(handle, rejectFrames = false)
        assertEquals(0L, stoppedRuntime.carrierActive)
        assertEquals("runtime_stopped", stoppedRuntime.lastError)

        FpsNative.startRuntime(handle)
        val started = FpsNativeTestHooks.startFakeCarrier(handle, rejectFrames = false)
        val startedAgain = FpsNativeTestHooks.startFakeCarrier(handle, rejectFrames = false)
        val stopped = FpsNativeTestHooks.stopFakeCarrier(handle)
        val stoppedAgain = FpsNativeTestHooks.stopFakeCarrier(handle)

        assertEquals(1L, started.carrierActive)
        assertEquals(1L, started.carrierStarted)
        assertEquals(null, started.lastError)
        assertEquals(1L, startedAgain.carrierActive)
        assertEquals(1L, startedAgain.carrierStarted)
        assertEquals(0L, startedAgain.carrierStopped)
        assertEquals(0L, stopped.carrierActive)
        assertEquals(1L, stopped.carrierStarted)
        assertEquals(1L, stopped.carrierStopped)
        assertEquals(0L, stoppedAgain.carrierActive)
        assertEquals(1L, stoppedAgain.carrierStopped)

        FpsNative.closeRuntime(handle)
    }

    @Test
    fun nativeRawCarrierRequiresStartedRuntimeAndRejectsInvalidEndpoint() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        val stopped = FpsNative.prepareRawCarrierSocket(handle, "127.0.0.1", 443)
        assertEquals(-1, stopped.rawCarrierProtectFd)
        assertEquals("runtime_stopped", stopped.lastError)

        FpsNative.startRuntime(handle)
        val badAddress = FpsNative.prepareRawCarrierSocket(handle, "not-an-ip-address", 443)
        val badPort = FpsNative.prepareRawCarrierSocket(handle, "127.0.0.1", 0)

        assertEquals(-1, badAddress.rawCarrierProtectFd)
        assertEquals("invalid_carrier_endpoint", badAddress.lastError)
        assertEquals(-1, badPort.rawCarrierProtectFd)
        assertEquals("invalid_carrier_endpoint", badPort.lastError)

        FpsNative.closeRuntime(handle)
    }

    @Test
    fun nativeRawCarrierCompleteBeforePrepareAndStopAreIdempotent() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        val stoppedBeforeStart = FpsNative.stopRawCarrier(handle)
        assertEquals(-1, stoppedBeforeStart.rawCarrierProtectFd)
        assertFalse(stoppedBeforeStart.rawCarrierActive)
        assertEquals(null, stoppedBeforeStart.lastError)

        FpsNative.startRuntime(handle)
        val notPrepared = FpsNative.completeRawCarrierProtection(handle, protectAllowed = true)
        val stoppedBeforePrepare = FpsNative.stopRawCarrier(handle)
        val prepared = FpsNative.prepareRawCarrierSocket(handle, "127.0.0.1", 443)
        val stoppedAfterPrepare = FpsNative.stopRawCarrier(handle)
        val stoppedAgain = FpsNative.stopRawCarrier(handle)

        assertEquals(-1, notPrepared.rawCarrierProtectFd)
        assertFalse(notPrepared.rawCarrierActive)
        assertEquals(0L, notPrepared.rawCarrierConnectAttempted)
        assertEquals(0L, notPrepared.rawCarrierConnectSucceeded)
        assertEquals(0L, notPrepared.rawCarrierConnectFailed)
        assertEquals("raw_carrier_not_prepared", notPrepared.lastError)
        assertEquals(-1, stoppedBeforePrepare.rawCarrierProtectFd)
        assertFalse(stoppedBeforePrepare.rawCarrierActive)
        assertEquals(null, stoppedBeforePrepare.lastError)
        assertTrue(prepared.rawCarrierProtectFd >= 0)
        assertEquals(null, prepared.lastError)
        assertEquals(-1, stoppedAfterPrepare.rawCarrierProtectFd)
        assertFalse(stoppedAfterPrepare.rawCarrierActive)
        assertEquals(null, stoppedAfterPrepare.lastError)
        assertEquals(-1, stoppedAgain.rawCarrierProtectFd)
        assertFalse(stoppedAgain.rawCarrierActive)
        assertEquals(null, stoppedAgain.lastError)

        FpsNative.closeRuntime(handle)
    }

    @Test
    fun nativeRawCarrierProtectFailureAbortsBeforeConnect() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)

        val prepared = FpsNative.prepareRawCarrierSocket(handle, "127.0.0.1", 443)
        val rejected = FpsNative.completeRawCarrierProtection(handle, protectAllowed = false)

        assertTrue(prepared.rawCarrierProtectFd >= 0)
        assertFalse(prepared.rawCarrierActive)
        assertEquals(-1, rejected.rawCarrierProtectFd)
        assertFalse(rejected.rawCarrierConnecting)
        assertFalse(rejected.rawCarrierActive)
        assertEquals(0L, rejected.rawCarrierConnectAttempted)
        assertEquals(0L, rejected.rawCarrierConnectSucceeded)
        assertEquals(1L, rejected.rawCarrierConnectFailed)
        assertEquals("socket_protect_failed", rejected.lastError)

        FpsNative.closeRuntime(handle)
    }

    @Test
    fun nativeRawCarrierConnectsToLoopbackTcpServerAndStops() {
        val server = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))
        val accepted = thread(start = true) {
            try {
                server.accept().use {
                    Thread.sleep(100)
                }
            } catch (_: Exception) {
                // Test cleanup closes the server socket if native connect fails.
            }
        }
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        try {
            FpsNative.startRuntime(handle)
            val prepared = FpsNative.prepareRawCarrierSocket(handle, "127.0.0.1", server.localPort)
            val connected = FpsNative.completeRawCarrierProtection(handle, protectAllowed = true)
            val stopped = FpsNative.stopRawCarrier(handle)
            val stoppedAgain = FpsNative.stopRawCarrier(handle)

            assertTrue(prepared.rawCarrierProtectFd >= 0)
            assertFalse(prepared.rawCarrierConnecting)
            assertFalse(prepared.rawCarrierActive)
            assertEquals(null, prepared.lastError)
            assertEquals(-1, connected.rawCarrierProtectFd)
            assertFalse(connected.rawCarrierConnecting)
            assertTrue(connected.rawCarrierActive)
            assertEquals(1L, connected.rawCarrierConnectAttempted)
            assertEquals(1L, connected.rawCarrierConnectSucceeded)
            assertEquals(0L, connected.rawCarrierConnectFailed)
            assertEquals(null, connected.lastError)
            assertEquals(-1, stopped.rawCarrierProtectFd)
            assertFalse(stopped.rawCarrierConnecting)
            assertFalse(stopped.rawCarrierActive)
            assertEquals(-1, stoppedAgain.rawCarrierProtectFd)
            assertFalse(stoppedAgain.rawCarrierConnecting)
            assertFalse(stoppedAgain.rawCarrierActive)
        } finally {
            FpsNative.closeRuntime(handle)
            server.close()
            accepted.join(1_000)
        }
    }

    @Test
    fun nativeTunPolicyAllowRoutesThroughFakeCarrierTransport() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        FpsNative.startTunPump(handle)
        FpsNativeTestHooks.startFakeCarrier(handle, rejectFrames = false)

        val packet = validUdpPacket()
        val output = FileOutputStream(writeEnd.fileDescriptor)
        output.write(packet)
        output.flush()
        awaitSnapshot(handle) { it.tunPolicyPending == 1L }
        val pending = FpsNative.drainTunPolicyPackets(handle, 1).single()
        val allowed = FpsNative.completeTunPolicyPacket(handle, pending.packetId, SplitTunnelDecision.ALLOW)
        val frames = FpsNativeTestHooks.capturedFakeCarrierFrameDigests(handle)

        assertEquals(1L, allowed.carrierActive)
        assertEquals(1L, allowed.carrierStarted)
        assertEquals(1L, allowed.tunPolicyAllowed)
        assertEquals(0L, allowed.tunPacketsDropped)
        assertEquals(1L, allowed.tunCovertEnqueueAttempted)
        assertEquals(1L, allowed.tunCovertEnqueueAccepted)
        assertEquals(0L, allowed.tunCovertEnqueueRejected)
        assertEquals(1L, allowed.carrierFramesEnqueued)
        assertEquals(packet.size.toLong(), allowed.carrierFrameBytesEnqueued)
        assertEquals(0L, allowed.carrierEnqueueRejected)
        assertEquals(null, allowed.lastError)
        assertEquals(listOf("client_to_server|opaque_datagram|${packet.size}|${sha256Hex(packet)}"), frames)

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
    }

    @Test
    fun nativeTunPolicyAllowReportsFakeCarrierReject() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        FpsNative.startTunPump(handle)
        FpsNativeTestHooks.startFakeCarrier(handle, rejectFrames = true)

        val output = FileOutputStream(writeEnd.fileDescriptor)
        output.write(validUdpPacket())
        output.flush()
        awaitSnapshot(handle) { it.tunPolicyPending == 1L }
        val pending = FpsNative.drainTunPolicyPackets(handle, 1).single()
        val rejected = FpsNative.completeTunPolicyPacket(handle, pending.packetId, SplitTunnelDecision.ALLOW)

        assertEquals(1L, rejected.carrierActive)
        assertEquals(1L, rejected.tunPolicyAllowed)
        assertEquals(1L, rejected.tunPacketsDropped)
        assertEquals(1L, rejected.tunCovertEnqueueAttempted)
        assertEquals(0L, rejected.tunCovertEnqueueAccepted)
        assertEquals(1L, rejected.tunCovertEnqueueRejected)
        assertEquals(0L, rejected.carrierFramesEnqueued)
        assertEquals(1L, rejected.carrierEnqueueRejected)
        assertEquals("carrier_enqueue_rejected", rejected.tunLastDropReason)
        assertEquals("carrier_enqueue_rejected", rejected.lastError)
        assertEquals(emptyList<String>(), FpsNativeTestHooks.capturedFakeCarrierFrameDigests(handle))

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
    }

    @Test
    fun nativeTunPolicyAllowReportsCaptureSinkReject() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)
        FpsNative.startRuntime(handle)
        val pipe = ParcelFileDescriptor.createPipe()
        val readEnd = pipe[0]
        val writeEnd = pipe[1]
        FpsNative.attachTunFdOwnedDuplicate(handle, readEnd.fd, 1280)
        FpsNative.startTunPump(handle)
        FpsNativeTestHooks.installTunPacketCaptureSink(handle, rejectPackets = true)

        val output = FileOutputStream(writeEnd.fileDescriptor)
        output.write(validUdpPacket())
        output.flush()
        awaitSnapshot(handle) { it.tunPolicyPending == 1L }
        val pending = FpsNative.drainTunPolicyPackets(handle, 1).single()
        val rejected = FpsNative.completeTunPolicyPacket(handle, pending.packetId, SplitTunnelDecision.ALLOW)

        assertEquals(0L, rejected.tunPolicyPending)
        assertEquals(0L, rejected.tunPolicyInFlight)
        assertEquals(1L, rejected.tunPolicyAllowed)
        assertEquals(1L, rejected.tunPacketsDropped)
        assertEquals(1L, rejected.tunCovertEnqueueAttempted)
        assertEquals(0L, rejected.tunCovertEnqueueAccepted)
        assertEquals(1L, rejected.tunCovertEnqueueRejected)
        assertEquals("carrier_enqueue_rejected", rejected.tunLastDropReason)
        assertEquals("carrier_enqueue_rejected", rejected.lastError)
        assertEquals(emptyList<String>(), FpsNativeTestHooks.capturedTunPacketDigests(handle))

        FpsNative.stopTunPump(handle)
        FpsNative.closeRuntime(handle)
        readEnd.close()
        writeEnd.close()
    }

    private fun awaitSnapshot(handle: Long, predicate: (NativeRuntimeSnapshot) -> Boolean): NativeRuntimeSnapshot {
        var snapshot = FpsNative.runtimeSnapshot(handle)
        repeat(100) {
            if (predicate(snapshot)) {
                return snapshot
            }
            Thread.sleep(20)
            snapshot = FpsNative.runtimeSnapshot(handle)
        }
        return snapshot
    }

    private fun validUdpPacket() = byteArrayOf(
        0x45, 0x00, 0x00, 0x1c,
        0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00,
        0x0a, 0x42, 0x00, 0x02,
        0x5d.toByte(), 0xb8.toByte(), 0xd8.toByte(), 0x22,
        0xcf.toByte(), 0x08, 0x01, 0xbb.toByte(),
        0x00, 0x08, 0x00, 0x00,
    )

    private fun sha256Hex(bytes: ByteArray): String {
        return MessageDigest.getInstance("SHA-256")
            .digest(bytes)
            .joinToString(separator = "") { "%02x".format(it.toInt() and 0xff) }
    }
}
