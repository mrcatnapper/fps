package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.CarrierProbeRuntimeStatus
import org.fpsproject.client.runtime.CarrierProbeTransportFactory
import org.fpsproject.client.runtime.HeadlessVpnController
import org.fpsproject.client.runtime.ResolvedEndpoint
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.TunRuntimeSnapshot
import org.fpsproject.client.runtime.VpnRuntimeSnapshot
import org.fpsproject.client.runtime.VpnRuntimeState
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.TunFlowTuple

data class NativeVpnRuntimeSnapshot(
    val vpn: VpnRuntimeSnapshot,
    val native: NativeRuntimeSnapshot,
) {
    companion object {
        fun stopped() = NativeVpnRuntimeSnapshot(
            vpn = VpnRuntimeSnapshot(
                state = VpnRuntimeState.STOPPED,
                lastError = null,
                tun = TunRuntimeSnapshot(fdPresent = false, mtu = null),
                carrierProbes = emptyList(),
            ),
            native = NativeRuntimeSnapshot(
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
                lastError = "runtime_stopped",
            ),
        )
    }
}

class HeadlessNativeVpnRuntime private constructor(
    private val controller: HeadlessVpnController,
    private val nativeRuntime: FpsNativeRuntime,
) : AutoCloseable {
    companion object {
        fun create(
            profileText: String,
            hooks: AndroidPlatformHooks,
            backend: FpsNativeBackend = FpsNative,
        ): HeadlessNativeVpnRuntime {
            val profile = AndroidClientProfileParser.parse(profileText)
            val nativeRuntime = FpsNativeRuntime.createForValidatedProfile(profileText, profile, backend)
            return HeadlessNativeVpnRuntime(
                controller = HeadlessVpnController(profile, hooks),
                nativeRuntime = nativeRuntime,
            )
        }
    }

    val state: VpnRuntimeState
        get() = controller.state

    val lastError: String?
        get() = controller.lastError

    fun start(): VpnRuntimeState {
        val next = controller.start()
        if (next != VpnRuntimeState.WAITING_FOR_LEASE) {
            return next
        }
        val nativeSnapshot = nativeRuntime.start()
        if (!nativeSnapshot.alive || !nativeSnapshot.started || !nativeSnapshot.workerThreadRunning) {
            controller.fail("native_runtime_start_failed")
            return controller.state
        }
        return controller.state
    }

    fun onLeaseReceived(lease: TunLease): VpnRuntimeState {
        val next = controller.onLeaseReceived(lease)
        if (next != VpnRuntimeState.RUNNING) {
            return next
        }
        val tun = controller.tun
        if (tun == null) {
            controller.fail("tun_missing_after_lease")
            return controller.state
        }
        val nativeSnapshot = nativeRuntime.attachTun(tun)
        if (!nativeSnapshot.alive || !nativeSnapshot.tunAttached || nativeSnapshot.tunFdOwnership != TUN_FD_OWNERSHIP_OWNED_DUPLICATE) {
            controller.fail("native_tun_attach_failed", closeTun = true)
            return controller.state
        }
        val pumpSnapshot = nativeRuntime.startTunPump()
        if (!pumpSnapshot.alive || !pumpSnapshot.tunPumpRunning) {
            controller.fail("native_tun_pump_start_failed", closeTun = true)
            return controller.state
        }
        return controller.state
    }

    fun prepareCarrierSocket(fd: Int): Boolean = controller.prepareCarrierSocket(fd)

    fun resolveServerEndpoint(): List<ResolvedEndpoint> = controller.resolveServerEndpoint()

    fun startNativeCarrier(endpoint: ResolvedEndpoint): NativeRuntimeSnapshot {
        val prepared = nativeRuntime.prepareRawCarrierSocket(endpoint.address, endpoint.port)
        if (!prepared.alive || prepared.rawCarrierProtectFd < 0) {
            if (prepared.lastError != null) {
                controller.fail(prepared.lastError)
            }
            return prepared
        }
        val protected = controller.prepareCarrierSocket(prepared.rawCarrierProtectFd)
        val completed = nativeRuntime.completeRawCarrierProtection(protected)
        if (!completed.rawCarrierActive && completed.lastError != null) {
            controller.fail(completed.lastError)
        }
        return completed
    }

    fun stopNativeCarrier(): NativeRuntimeSnapshot = nativeRuntime.stopRawCarrier()

    fun startCarrierProbeRunners(transportFactory: CarrierProbeTransportFactory, nowMs: Long = 0): List<CarrierProbeRuntimeStatus> {
        return controller.startCarrierProbeRunners(transportFactory, nowMs)
    }

    fun tickCarrierProbeRunners(nowMs: Long): List<CarrierProbeRuntimeStatus> = controller.tickCarrierProbeRunners(nowMs)

    fun stopCarrierProbeRunners(): List<CarrierProbeRuntimeStatus> = controller.stopCarrierProbeRunners()

    fun policyDecision(flow: TunFlowTuple?): SplitTunnelDecision = controller.policyDecision(flow)

    fun drainTunPolicyPackets(maxPackets: Int = 64): List<NativeTunPolicyPacket> = nativeRuntime.drainTunPolicyPackets(maxPackets)

    fun applyPendingTunPolicy(maxPackets: Int = 64): NativeRuntimeSnapshot {
        var snapshot = nativeRuntime.snapshot()
        for (packet in drainTunPolicyPackets(maxPackets)) {
            snapshot = nativeRuntime.completeTunPolicyPacket(packet.packetId, policyDecision(packet.flow))
        }
        return snapshot
    }

    fun snapshot(): NativeVpnRuntimeSnapshot {
        return NativeVpnRuntimeSnapshot(
            vpn = controller.snapshot(),
            native = nativeRuntime.snapshot(),
        )
    }

    fun stop(): VpnRuntimeState {
        nativeRuntime.stopRawCarrier()
        nativeRuntime.stopTunPump()
        nativeRuntime.stop()
        return controller.stop()
    }

    override fun close() {
        stop()
        nativeRuntime.close()
    }
}
