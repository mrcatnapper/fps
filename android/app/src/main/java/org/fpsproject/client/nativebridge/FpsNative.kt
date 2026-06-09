package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.policy.TunFlowTuple

const val TUN_FD_OWNERSHIP_OWNED_DUPLICATE = "owned_duplicate"

data class NativeTunPolicyPacket(
    val packetId: Long,
    val packetSize: Int,
    val flow: TunFlowTuple,
)

data class NativeRuntimeSnapshot(
    val alive: Boolean,
    val started: Boolean,
    val workerThreadRunning: Boolean,
    val tunAttached: Boolean,
    val tunPumpRunning: Boolean,
    val tunFd: Int,
    val tunMtu: Int,
    val tunFdOwnership: String?,
    val tunPacketsRead: Long,
    val tunBytesRead: Long,
    val tunPacketsParsed: Long,
    val tunPacketsDropped: Long,
    val tunLastDropReason: String?,
    val tunPolicyPending: Long,
    val tunPolicyInFlight: Long,
    val tunPolicyAllowed: Long,
    val tunPolicyDropped: Long,
    val tunPolicyQueueFull: Long,
    val tunCovertEnqueueAttempted: Long,
    val tunCovertEnqueueAccepted: Long,
    val tunCovertEnqueueRejected: Long,
    val commandsPosted: Long,
    val commandsCompleted: Long,
    val carrierActive: Long = 0,
    val carrierStarted: Long = 0,
    val carrierStopped: Long = 0,
    val carrierFramesEnqueued: Long = 0,
    val carrierFrameBytesEnqueued: Long = 0,
    val carrierEnqueueRejected: Long = 0,
    val rawCarrierProtectFd: Int = -1,
    val rawCarrierConnecting: Boolean = false,
    val rawCarrierActive: Boolean = false,
    val rawCarrierConnectAttempted: Long = 0,
    val rawCarrierConnectSucceeded: Long = 0,
    val rawCarrierConnectFailed: Long = 0,
    val lastError: String?,
)

interface FpsNativeBackend {
    fun createRuntime(profileText: String): Long

    fun closeRuntime(handle: Long)

    fun startRuntime(handle: Long): NativeRuntimeSnapshot

    fun stopRuntime(handle: Long): NativeRuntimeSnapshot

    fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot

    fun startTunPump(handle: Long): NativeRuntimeSnapshot

    fun stopTunPump(handle: Long): NativeRuntimeSnapshot

    fun postNoopCommand(handle: Long): NativeRuntimeSnapshot

    fun attachTunFdOwnedDuplicate(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot

    fun drainTunPolicyPackets(handle: Long, maxPackets: Int): List<NativeTunPolicyPacket>

    fun completeTunPolicyPacket(handle: Long, packetId: Long, decision: SplitTunnelDecision): NativeRuntimeSnapshot

    fun prepareRawCarrierSocket(handle: Long, address: String, port: Int): NativeRuntimeSnapshot

    fun completeRawCarrierProtection(handle: Long, protectAllowed: Boolean): NativeRuntimeSnapshot

    fun stopRawCarrier(handle: Long): NativeRuntimeSnapshot
}

class FpsNativeRuntime private constructor(
    private var handle: Long,
    private val backend: FpsNativeBackend,
) : AutoCloseable {
    companion object {
        fun create(profileText: String, backend: FpsNativeBackend = FpsNative): FpsNativeRuntime {
            val profile = AndroidClientProfileParser.parse(profileText)
            return createForValidatedProfile(profileText, profile, backend)
        }

        internal fun createForValidatedProfile(
            profileText: String,
            @Suppress("UNUSED_PARAMETER") profile: AndroidClientProfile,
            backend: FpsNativeBackend = FpsNative,
        ): FpsNativeRuntime {
            val handle = backend.createRuntime(profileText)
            require(handle != 0L) { "native runtime creation failed" }
            return FpsNativeRuntime(handle, backend)
        }
    }

    fun snapshot(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return NativeRuntimeSnapshot(
                alive = false,
                started = false,
                workerThreadRunning = false,
                tunAttached = false,
                tunPumpRunning = false,
                tunFd = -1,
                tunMtu = 0,
                tunFdOwnership = null,
                tunPacketsRead = 0,
                tunBytesRead = 0,
                tunPacketsParsed = 0,
                tunPacketsDropped = 0,
                tunLastDropReason = null,
                tunPolicyPending = 0,
                tunPolicyInFlight = 0,
                tunPolicyAllowed = 0,
                tunPolicyDropped = 0,
                tunPolicyQueueFull = 0,
                tunCovertEnqueueAttempted = 0,
                tunCovertEnqueueAccepted = 0,
                tunCovertEnqueueRejected = 0,
                commandsPosted = 0,
                commandsCompleted = 0,
                lastError = "runtime_closed",
            )
        }
        return backend.runtimeSnapshot(activeHandle)
    }

    fun startTunPump(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.startTunPump(activeHandle)
    }

    fun stopTunPump(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.stopTunPump(activeHandle)
    }

    fun start(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.startRuntime(activeHandle)
    }

    fun stop(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.stopRuntime(activeHandle)
    }

    fun postNoopCommand(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.postNoopCommand(activeHandle)
    }

    fun attachTun(tun: EstablishedTun): NativeRuntimeSnapshot = attachTunFdOwnedDuplicate(tun.fd, tun.mtu)

    fun attachTunFdOwnedDuplicate(fd: Int, mtu: Int): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.attachTunFdOwnedDuplicate(activeHandle, fd, mtu)
    }

    fun drainTunPolicyPackets(maxPackets: Int): List<NativeTunPolicyPacket> {
        val activeHandle = handle
        if (activeHandle == 0L || maxPackets <= 0) {
            return emptyList()
        }
        return backend.drainTunPolicyPackets(activeHandle, maxPackets)
    }

    fun completeTunPolicyPacket(packetId: Long, decision: SplitTunnelDecision): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.completeTunPolicyPacket(activeHandle, packetId, decision)
    }

    fun prepareRawCarrierSocket(address: String, port: Int): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.prepareRawCarrierSocket(activeHandle, address, port)
    }

    fun completeRawCarrierProtection(protectAllowed: Boolean): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.completeRawCarrierProtection(activeHandle, protectAllowed)
    }

    fun stopRawCarrier(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.stopRawCarrier(activeHandle)
    }

    override fun close() {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return
        }
        handle = 0
        backend.closeRuntime(activeHandle)
    }
}

object FpsNative : FpsNativeBackend {
    init {
        System.loadLibrary("fps_android_native")
    }

    external fun nativeVersion(): String

    external fun nativeCoreSmoke(): String

    external fun parseIpv4FlowTuple(packet: ByteArray): TunFlowTuple?

    external override fun createRuntime(profileText: String): Long

    external override fun closeRuntime(handle: Long)

    external override fun startRuntime(handle: Long): NativeRuntimeSnapshot

    external override fun stopRuntime(handle: Long): NativeRuntimeSnapshot

    external override fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot

    external override fun startTunPump(handle: Long): NativeRuntimeSnapshot

    external override fun stopTunPump(handle: Long): NativeRuntimeSnapshot

    external override fun postNoopCommand(handle: Long): NativeRuntimeSnapshot

    external override fun attachTunFdOwnedDuplicate(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot

    external fun nativeDrainTunPolicyPackets(handle: Long, maxPackets: Int): Array<NativeTunPolicyPacket>

    override fun drainTunPolicyPackets(handle: Long, maxPackets: Int): List<NativeTunPolicyPacket> {
        return nativeDrainTunPolicyPackets(handle, maxPackets).toList()
    }

    external fun nativeCompleteTunPolicyPacket(handle: Long, packetId: Long, allow: Boolean): NativeRuntimeSnapshot

    override fun completeTunPolicyPacket(handle: Long, packetId: Long, decision: SplitTunnelDecision): NativeRuntimeSnapshot {
        return nativeCompleteTunPolicyPacket(handle, packetId, decision == SplitTunnelDecision.ALLOW)
    }

    external override fun prepareRawCarrierSocket(handle: Long, address: String, port: Int): NativeRuntimeSnapshot

    external override fun completeRawCarrierProtection(handle: Long, protectAllowed: Boolean): NativeRuntimeSnapshot

    external override fun stopRawCarrier(handle: Long): NativeRuntimeSnapshot

}
