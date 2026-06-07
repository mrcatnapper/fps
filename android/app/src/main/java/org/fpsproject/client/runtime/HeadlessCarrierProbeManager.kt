package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.config.CarrierProbeMode

enum class CarrierProbeRuntimeState {
    STOPPED,
    RESOLVING,
    PROTECTING,
    CONNECTING,
    RUNNING,
    BACKOFF,
    FAILED,
}

data class CarrierProbeRuntimeStatus(
    val id: Int,
    val mode: CarrierProbeMode,
    val state: CarrierProbeRuntimeState,
    val attempts: Int,
    val successfulProbes: Int,
    val reconnects: Int,
    val lastError: String?,
    val nextRetryDelayMs: Long,
)

class CarrierProbeResult private constructor(
    val ok: Boolean,
    val error: String?,
) {
    companion object {
        fun success() = CarrierProbeResult(ok = true, error = null)

        fun failure(error: String) = CarrierProbeResult(ok = false, error = error)
    }
}

interface CarrierProbeTransport {
    val socketFd: Int
    val protectsSocketsInternally: Boolean
        get() = false

    fun connect(endpoint: ResolvedEndpoint): CarrierProbeResult

    fun probe(nowMs: Long): CarrierProbeResult

    fun close()
}

fun interface CarrierProbeTransportFactory {
    fun create(plan: CarrierProbeRuntimePlan): CarrierProbeTransport
}

class HeadlessCarrierProbeManager(
    profile: AndroidClientProfile,
    private val hooks: AndroidPlatformHooks,
    private val transportFactory: CarrierProbeTransportFactory,
    private val initialBackoffMs: Long = 1000,
    private val maxBackoffMs: Long = 30_000,
) {
    private val runners = profile.carriers.mapIndexed { index, probe ->
        CarrierRunner(
            plan = CarrierProbeRuntimePlan(id = index, probe = probe),
            hooks = hooks,
            transportFactory = transportFactory,
            initialBackoffMs = initialBackoffMs,
            maxBackoffMs = maxBackoffMs,
        )
    }

    fun start(nowMs: Long = 0): List<CarrierProbeRuntimeStatus> {
        runners.forEach { it.start(nowMs) }
        return statuses()
    }

    fun tick(nowMs: Long): List<CarrierProbeRuntimeStatus> {
        runners.forEach { it.tick(nowMs) }
        return statuses()
    }

    fun stop(): List<CarrierProbeRuntimeStatus> {
        runners.forEach { it.stop() }
        return statuses()
    }

    fun statuses(): List<CarrierProbeRuntimeStatus> = runners.map { it.status() }
}

private class CarrierRunner(
    private val plan: CarrierProbeRuntimePlan,
    private val hooks: AndroidPlatformHooks,
    private val transportFactory: CarrierProbeTransportFactory,
    private val initialBackoffMs: Long,
    private val maxBackoffMs: Long,
) {
    private var state = CarrierProbeRuntimeState.STOPPED
    private var transport: CarrierProbeTransport? = null
    private var attempts = 0
    private var successfulProbes = 0
    private var reconnects = 0
    private var lastError: String? = null
    private var backoffMs = initialBackoffMs
    private var nextRetryAtMs = 0L
    private var nextRetryDelayMs = 0L
    private var nextProbeAtMs = Long.MAX_VALUE

    fun start(nowMs: Long) {
        if (state != CarrierProbeRuntimeState.STOPPED) {
            return
        }
        state = CarrierProbeRuntimeState.RESOLVING
        resolveAndConnect(nowMs)
    }

    fun tick(nowMs: Long) {
        when (state) {
            CarrierProbeRuntimeState.RUNNING -> {
                if (nowMs >= nextProbeAtMs) {
                    probe(nowMs)
                }
            }
            CarrierProbeRuntimeState.BACKOFF -> {
                if (nowMs >= nextRetryAtMs) {
                    state = CarrierProbeRuntimeState.RESOLVING
                    resolveAndConnect(nowMs)
                }
            }
            CarrierProbeRuntimeState.RESOLVING,
            CarrierProbeRuntimeState.PROTECTING,
            CarrierProbeRuntimeState.CONNECTING,
            CarrierProbeRuntimeState.STOPPED,
            CarrierProbeRuntimeState.FAILED,
            -> Unit
        }
    }

    fun stop() {
        transport?.close()
        transport = null
        state = CarrierProbeRuntimeState.STOPPED
        nextRetryDelayMs = 0
        nextRetryAtMs = 0
        nextProbeAtMs = Long.MAX_VALUE
    }

    fun status() = CarrierProbeRuntimeStatus(
        id = plan.id,
        mode = plan.probe.mode,
        state = state,
        attempts = attempts,
        successfulProbes = successfulProbes,
        reconnects = reconnects,
        lastError = lastError,
        nextRetryDelayMs = nextRetryDelayMs,
    )

    private fun resolveAndConnect(nowMs: Long) {
        state = CarrierProbeRuntimeState.RESOLVING
        val endpoints = hooks.resolveOnUnderlyingNetwork(plan.probe.endpoint.host, plan.probe.endpoint.port)
        if (endpoints.isEmpty()) {
            enterBackoff("resolve_empty", nowMs)
            return
        }

        val candidate = transportFactory.create(plan)
        transport = candidate

        state = CarrierProbeRuntimeState.PROTECTING
        if (!candidate.protectsSocketsInternally && !hooks.protectSocket(candidate.socketFd)) {
            candidate.close()
            transport = null
            enterBackoff("socket_protect_failed", nowMs)
            return
        }

        state = CarrierProbeRuntimeState.CONNECTING
        attempts += 1
        val connected = candidate.connect(endpoints.first())
        if (!connected.ok) {
            candidate.close()
            transport = null
            enterBackoff(connected.error ?: "connect_failed", nowMs)
            return
        }

        state = CarrierProbeRuntimeState.RUNNING
        lastError = null
        backoffMs = initialBackoffMs
        nextRetryDelayMs = 0
        nextProbeAtMs = nowMs
    }

    private fun probe(nowMs: Long) {
        val active = transport
        if (active == null) {
            enterBackoff("transport_missing", nowMs)
            return
        }

        val result = active.probe(nowMs)
        if (result.ok) {
            successfulProbes += 1
            nextProbeAtMs = nowMs + plan.probe.intervalMs
            return
        }

        active.close()
        transport = null
        reconnects += 1
        enterBackoff(result.error ?: "probe_failed", nowMs)
    }

    private fun enterBackoff(error: String, nowMs: Long) {
        state = CarrierProbeRuntimeState.BACKOFF
        lastError = error
        nextRetryDelayMs = backoffMs
        nextRetryAtMs = nowMs + backoffMs
        backoffMs = (backoffMs * 2).coerceAtMost(maxBackoffMs)
    }
}
