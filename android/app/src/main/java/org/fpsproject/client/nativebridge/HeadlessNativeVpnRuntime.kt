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
                tunAttached = false,
                tunFd = -1,
                tunMtu = 0,
                tunFdOwnership = null,
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

    fun start(): VpnRuntimeState = controller.start()

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
        return controller.state
    }

    fun prepareCarrierSocket(fd: Int): Boolean = controller.prepareCarrierSocket(fd)

    fun resolveServerEndpoint(): List<ResolvedEndpoint> = controller.resolveServerEndpoint()

    fun startCarrierProbeRunners(transportFactory: CarrierProbeTransportFactory, nowMs: Long = 0): List<CarrierProbeRuntimeStatus> {
        return controller.startCarrierProbeRunners(transportFactory, nowMs)
    }

    fun tickCarrierProbeRunners(nowMs: Long): List<CarrierProbeRuntimeStatus> = controller.tickCarrierProbeRunners(nowMs)

    fun stopCarrierProbeRunners(): List<CarrierProbeRuntimeStatus> = controller.stopCarrierProbeRunners()

    fun policyDecision(flow: TunFlowTuple?): SplitTunnelDecision = controller.policyDecision(flow)

    fun snapshot(): NativeVpnRuntimeSnapshot {
        return NativeVpnRuntimeSnapshot(
            vpn = controller.snapshot(),
            native = nativeRuntime.snapshot(),
        )
    }

    fun stop(): VpnRuntimeState {
        nativeRuntime.close()
        return controller.stop()
    }

    override fun close() {
        stop()
    }
}
