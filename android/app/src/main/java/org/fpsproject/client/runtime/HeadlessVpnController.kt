package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.config.CarrierProbeProfile
import org.fpsproject.client.policy.SplitTunnelDecision
import org.fpsproject.client.policy.SplitTunnelPolicy
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.UidResolver
import java.net.Socket

enum class VpnRuntimeState {
    STOPPED,
    NEEDS_VPN_PERMISSION,
    WAITING_FOR_LEASE,
    RUNNING,
    FAILED,
}

data class TunLease(
    val clientIpv4: Long,
    val serverIpv4: Long,
    val prefixLength: Int,
    val mtu: Int,
) {
    init {
        require(prefixLength in 0..32) { "prefixLength must be in 0..32" }
        require(mtu > 0) { "mtu must be positive" }
    }
}

data class EstablishedTun(
    val fd: Int,
    val mtu: Int,
) {
    companion object {
        fun owned(fd: Int, mtu: Int, handle: TunHandle): EstablishedTun {
            return EstablishedTun(fd = fd, mtu = mtu).also {
                it.closeAction = handle::close
            }
        }

        fun borrowed(fd: Int, mtu: Int): EstablishedTun {
            return EstablishedTun(fd = fd, mtu = mtu)
        }
    }

    private var closeAction: (() -> Unit)? = null

    fun close() {
        val action = closeAction ?: return
        closeAction = null
        action()
    }
}

data class ResolvedEndpoint(
    val address: String,
    val port: Int,
)

data class CarrierProbeRuntimePlan(
    val id: Int,
    val probe: CarrierProbeProfile,
)

data class TunRuntimeSnapshot(
    val fdPresent: Boolean,
    val mtu: Int?,
)

data class VpnRuntimeSnapshot(
    val state: VpnRuntimeState,
    val lastError: String?,
    val tun: TunRuntimeSnapshot,
    val carrierProbes: List<CarrierProbeRuntimeStatus>,
)

interface AndroidPlatformHooks {
    fun hasVpnPermission(): Boolean

    fun establishTun(profile: AndroidClientProfile, lease: TunLease): EstablishedTun?

    fun protectSocket(fd: Int): Boolean

    fun protectSocket(socket: Socket): Boolean

    fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint>

    fun uidForFlow(flow: TunFlowTuple): Int
}

class PlatformUidResolver(private val hooks: AndroidPlatformHooks) : UidResolver {
    override fun ownerUidFor(flow: TunFlowTuple): Int = hooks.uidForFlow(flow)
}

class HeadlessVpnController(
    private val profile: AndroidClientProfile,
    private val hooks: AndroidPlatformHooks,
) {
    private val carrierProbePlans = profile.carriers.mapIndexed { index, probe ->
        CarrierProbeRuntimePlan(id = index, probe = probe)
    }
    private val splitTunnelPolicy = SplitTunnelPolicy(profile.splitTunnel.allowedUids, PlatformUidResolver(hooks))
    private var carrierProbeManager: HeadlessCarrierProbeManager? = null

    var state: VpnRuntimeState = VpnRuntimeState.STOPPED
        private set

    var lastError: String? = null
        private set

    var tun: EstablishedTun? = null
        private set

    fun start(): VpnRuntimeState {
        lastError = null
        if (!hooks.hasVpnPermission()) {
            state = VpnRuntimeState.NEEDS_VPN_PERMISSION
            return state
        }
        state = VpnRuntimeState.WAITING_FOR_LEASE
        return state
    }

    fun onLeaseReceived(lease: TunLease): VpnRuntimeState {
        if (state != VpnRuntimeState.WAITING_FOR_LEASE) {
            fail("lease_unexpected")
            return state
        }
        if (profile.tun?.enabled != true) {
            fail("tun_disabled")
            return state
        }
        val established = hooks.establishTun(profile, lease)
        if (established == null) {
            fail("tun_establish_failed")
            return state
        }
        tun = established
        state = VpnRuntimeState.RUNNING
        lastError = null
        return state
    }

    fun prepareCarrierSocket(fd: Int): Boolean {
        if (!hooks.protectSocket(fd)) {
            fail("socket_protect_failed")
            return false
        }
        return true
    }

    fun carrierProbePlans(): List<CarrierProbeRuntimePlan> = carrierProbePlans.toList()

    fun resolveServerEndpoint(): List<ResolvedEndpoint> {
        return hooks.resolveOnUnderlyingNetwork(profile.server.host, profile.server.port)
    }

    fun resolveCarrierEndpoint(plan: CarrierProbeRuntimePlan): List<ResolvedEndpoint> {
        return hooks.resolveOnUnderlyingNetwork(plan.probe.endpoint.host, plan.probe.endpoint.port)
    }

    fun startCarrierProbeRunners(transportFactory: CarrierProbeTransportFactory, nowMs: Long = 0): List<CarrierProbeRuntimeStatus> {
        if (carrierProbeManager == null) {
            carrierProbeManager = HeadlessCarrierProbeManager(profile, hooks, transportFactory)
        }
        return carrierProbeManager!!.start(nowMs)
    }

    fun tickCarrierProbeRunners(nowMs: Long): List<CarrierProbeRuntimeStatus> {
        return carrierProbeManager?.tick(nowMs) ?: emptyList()
    }

    fun stopCarrierProbeRunners(): List<CarrierProbeRuntimeStatus> {
        val stopped = carrierProbeManager?.stop() ?: emptyList()
        carrierProbeManager = null
        return stopped
    }

    fun policyDecision(flow: TunFlowTuple?): SplitTunnelDecision {
        return splitTunnelPolicy.decide(flow)
    }

    fun snapshot(): VpnRuntimeSnapshot {
        val activeTun = tun
        return VpnRuntimeSnapshot(
            state = state,
            lastError = lastError,
            tun = TunRuntimeSnapshot(fdPresent = activeTun != null, mtu = activeTun?.mtu),
            carrierProbes = carrierProbeManager?.statuses() ?: emptyList(),
        )
    }

    fun stop(): VpnRuntimeState {
        stopCarrierProbeRunners()
        tun?.close()
        tun = null
        lastError = null
        state = VpnRuntimeState.STOPPED
        return state
    }

    private fun fail(error: String) {
        lastError = error
        state = VpnRuntimeState.FAILED
    }
}
