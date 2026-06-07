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
)

data class ResolvedEndpoint(
    val address: String,
    val port: Int,
)

data class CarrierRuntimePlan(
    val id: Int,
    val probe: CarrierProbeProfile,
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
    private val carrierPlans = profile.carriers.mapIndexed { index, probe ->
        CarrierRuntimePlan(id = index, probe = probe)
    }
    private val splitTunnelPolicy = SplitTunnelPolicy(profile.splitTunnel.allowedUids, PlatformUidResolver(hooks))
    private var carrierManager: HeadlessCarrierManager? = null

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

    fun carrierPlans(): List<CarrierRuntimePlan> = carrierPlans.toList()

    fun resolveServerEndpoint(): List<ResolvedEndpoint> {
        return hooks.resolveOnUnderlyingNetwork(profile.server.host, profile.server.port)
    }

    fun resolveCarrierEndpoint(plan: CarrierRuntimePlan): List<ResolvedEndpoint> {
        return hooks.resolveOnUnderlyingNetwork(plan.probe.endpoint.host, plan.probe.endpoint.port)
    }

    fun startCarrierRunners(transportFactory: CarrierTransportFactory, nowMs: Long = 0): List<CarrierRuntimeStatus> {
        if (carrierManager == null) {
            carrierManager = HeadlessCarrierManager(profile, hooks, transportFactory)
        }
        return carrierManager!!.start(nowMs)
    }

    fun tickCarrierRunners(nowMs: Long): List<CarrierRuntimeStatus> {
        return carrierManager?.tick(nowMs) ?: emptyList()
    }

    fun stopCarrierRunners(): List<CarrierRuntimeStatus> {
        val stopped = carrierManager?.stop() ?: emptyList()
        carrierManager = null
        return stopped
    }

    fun policyDecision(flow: TunFlowTuple?): SplitTunnelDecision {
        return splitTunnelPolicy.decide(flow)
    }

    fun stop(): VpnRuntimeState {
        stopCarrierRunners()
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
