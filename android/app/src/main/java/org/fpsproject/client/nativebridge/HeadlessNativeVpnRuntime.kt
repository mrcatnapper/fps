package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.CarrierProbeRuntimePlan
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
                tunPacketsWritten = 0,
                tunBytesWritten = 0,
                tunInboundWriteRejected = 0,
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

interface LocalCoverClientHandle : AutoCloseable

data class LocalCoverClientStartResult(
    val handle: LocalCoverClientHandle?,
    val error: String?,
) {
    init {
        require((handle != null) != (error != null)) {
            "local cover client start result must contain either a handle or an error"
        }
    }

    companion object {
        fun started(handle: LocalCoverClientHandle = NoopLocalCoverClientHandle): LocalCoverClientStartResult {
            return LocalCoverClientStartResult(handle = handle, error = null)
        }

        fun failed(error: String = "cover_client_start_failed"): LocalCoverClientStartResult {
            return LocalCoverClientStartResult(handle = null, error = error.ifBlank { "cover_client_start_failed" })
        }
    }
}

fun interface LocalCoverClientStarter {
    fun start(localBridgePort: Int, carrierPlan: CarrierProbeRuntimePlan): LocalCoverClientStartResult
}

private object NoopLocalCoverClientHandle : LocalCoverClientHandle {
    override fun close() = Unit
}

class HeadlessNativeVpnRuntime private constructor(
    private val controller: HeadlessVpnController,
    private val nativeRuntime: FpsNativeRuntime,
) : AutoCloseable {
    private var activeCoverClient: LocalCoverClientHandle? = null

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

    fun startNativeCarrierBridge(): NativeRuntimeSnapshot = nativeRuntime.startRawCarrierBridge()

    fun startCoordinated(
        coverClientStarter: LocalCoverClientStarter,
        maxNativeEvents: Int = 16,
        maxPolicyPackets: Int = 64,
    ): NativeVpnRuntimeSnapshot {
        if (state != VpnRuntimeState.STOPPED) {
            return snapshot()
        }
        val started = start()
        if (started != VpnRuntimeState.WAITING_FOR_LEASE) {
            return snapshot()
        }
        val endpoints = runCatching { resolveServerEndpoint() }
            .getOrElse { return failCoordinated("server_resolve_failed") }
        val endpoint = endpoints.firstOrNull()
            ?: return failCoordinated("server_resolve_failed")
        val coverPlan = controller.carrierProbePlans().firstOrNull()
            ?: return failCoordinated("carrier_profile_missing")
        val carrier = startNativeCarrier(endpoint)
        if (!carrier.rawCarrierActive) {
            return failCoordinated(carrier.lastError ?: "raw_carrier_connect_failed")
        }
        val bridge = startNativeCarrierBridge()
        if (!bridge.rawCarrierBridgeListening || bridge.rawCarrierBridgeListenPort <= 0) {
            return failCoordinated(bridge.lastError ?: "raw_carrier_bridge_start_failed")
        }
        val cover = runCatching { coverClientStarter.start(bridge.rawCarrierBridgeListenPort, coverPlan) }
            .getOrElse { return failCoordinated("cover_client_start_failed") }
        val coverHandle = cover.handle
        if (coverHandle == null) {
            return failCoordinated(cover.error ?: "cover_client_start_failed")
        }
        activeCoverClient = coverHandle
        return tickCoordinated(maxNativeEvents = maxNativeEvents, maxPolicyPackets = maxPolicyPackets)
    }

    fun tickCoordinated(maxNativeEvents: Int = 16, maxPolicyPackets: Int = 64): NativeVpnRuntimeSnapshot {
        val next = applyNativeEvents(maxNativeEvents)
        if (next == VpnRuntimeState.FAILED) {
            return failCoordinated(controller.lastError ?: "native_event_failed")
        }
        if (next == VpnRuntimeState.RUNNING) {
            applyPendingTunPolicy(maxPolicyPackets)
        }
        return snapshot()
    }

    fun runClientAuthSmokeForTest(tamperServerAccept: Boolean = false): NativeRuntimeSnapshot {
        return nativeRuntime.runClientAuthSmokeForTest(tamperServerAccept)
    }

    fun drainNativeEvents(maxEvents: Int = 16): List<NativeRuntimeEvent> = nativeRuntime.drainNativeEvents(maxEvents)

    fun applyNativeEvents(maxEvents: Int = 16): VpnRuntimeState {
        for (event in drainNativeEvents(maxEvents)) {
            when (event.type) {
                NATIVE_EVENT_LEASE_RECEIVED -> {
                    val lease = event.tunLeaseOrNull()
                    if (lease == null) {
                        controller.fail("native_lease_event_invalid")
                        return controller.state
                    }
                    val next = onLeaseReceived(lease)
                    if (next == VpnRuntimeState.FAILED) {
                        return next
                    }
                }
                NATIVE_EVENT_CARRIER_AUTH_FAILED -> {
                    controller.fail(event.error ?: "native_carrier_auth_failed")
                    return controller.state
                }
            }
        }
        return controller.state
    }

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
        closeCoverClient()
        nativeRuntime.stopRawCarrier()
        nativeRuntime.stopTunPump()
        nativeRuntime.stop()
        return controller.stop()
    }

    override fun close() {
        stop()
        nativeRuntime.close()
    }

    private fun failCoordinated(error: String): NativeVpnRuntimeSnapshot {
        closeCoverClient()
        nativeRuntime.stopRawCarrier()
        nativeRuntime.stopTunPump()
        nativeRuntime.stop()
        controller.fail(error, closeTun = true)
        return snapshot()
    }

    private fun closeCoverClient() {
        val coverClient = activeCoverClient ?: return
        activeCoverClient = null
        runCatching { coverClient.close() }
    }
}
