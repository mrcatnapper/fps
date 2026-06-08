package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.ResolvedEndpoint
import org.fpsproject.client.runtime.TunHandle
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnRuntimeState
import org.fpsproject.client.policy.TunFlowTuple
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
}

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
    commandsPosted: Long = 0,
    commandsCompleted: Long = 0,
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
    commandsPosted = commandsPosted,
    commandsCompleted = commandsCompleted,
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
) : FpsNativeBackend {
    private var nextHandle = 1L
    val createdProfiles = mutableListOf<Pair<Long, String>>()
    val startedHandles = mutableListOf<Long>()
    val stoppedHandles = mutableListOf<Long>()
    val closedHandles = mutableListOf<Long>()
    val attachedTun = mutableListOf<Triple<Long, Int, Int>>()
    val startedTunPumps = mutableListOf<Long>()
    val stoppedTunPumps = mutableListOf<Long>()
    private val snapshots = mutableMapOf<Long, NativeRuntimeSnapshot>()

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
}

private class FakeAndroidHooks(
    private val vpnPermissionGranted: Boolean = true,
    private val establishedTun: EstablishedTun? = EstablishedTun.borrowed(fd = 7, mtu = 1280),
) : AndroidPlatformHooks {
    var establishTunCalls = 0

    override fun hasVpnPermission() = vpnPermissionGranted

    override fun establishTun(profile: AndroidClientProfile, lease: TunLease): EstablishedTun? {
        establishTunCalls += 1
        return establishedTun
    }

    override fun protectSocket(fd: Int) = true

    override fun protectSocket(socket: Socket) = true

    override fun resolveOnUnderlyingNetwork(host: String, port: Int) = listOf(ResolvedEndpoint("203.0.113.10", port))

    override fun uidForFlow(flow: TunFlowTuple) = -1
}

private class CountingTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}
