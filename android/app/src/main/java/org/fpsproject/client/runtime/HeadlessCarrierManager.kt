package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.config.CarrierProbeMode

enum class CarrierRuntimeState {
    STOPPED,
    RESOLVING,
    PROTECTING,
    CONNECTING,
    RUNNING,
    BACKOFF,
    FAILED,
}

data class CarrierRuntimeStatus(
    val id: Int,
    val mode: CarrierProbeMode,
    val state: CarrierRuntimeState,
    val attempts: Int,
    val successfulProbes: Int,
    val reconnects: Int,
    val lastError: String?,
    val nextRetryDelayMs: Long,
)

class CarrierTransportResult private constructor(
    val ok: Boolean,
    val error: String?,
) {
    companion object {
        fun success() = CarrierTransportResult(ok = true, error = null)

        fun failure(error: String) = CarrierTransportResult(ok = false, error = error)
    }
}

interface CarrierTransport {
    val socketFd: Int

    fun connect(endpoint: ResolvedEndpoint): CarrierTransportResult

    fun probe(nowMs: Long): CarrierTransportResult

    fun close()
}

fun interface CarrierTransportFactory {
    fun create(plan: CarrierRuntimePlan): CarrierTransport
}

class HeadlessCarrierManager(
    profile: AndroidClientProfile,
    private val hooks: AndroidPlatformHooks,
    private val transportFactory: CarrierTransportFactory,
    private val initialBackoffMs: Long = 1000,
    private val maxBackoffMs: Long = 30_000,
) {
    private val runners = profile.carriers.mapIndexed { index, probe ->
        CarrierRunner(
            plan = CarrierRuntimePlan(id = index, probe = probe),
            hooks = hooks,
            transportFactory = transportFactory,
            initialBackoffMs = initialBackoffMs,
            maxBackoffMs = maxBackoffMs,
        )
    }

    fun start(nowMs: Long = 0): List<CarrierRuntimeStatus> {
        runners.forEach { it.start(nowMs) }
        return statuses()
    }

    fun tick(nowMs: Long): List<CarrierRuntimeStatus> {
        runners.forEach { it.tick(nowMs) }
        return statuses()
    }

    fun stop(): List<CarrierRuntimeStatus> {
        runners.forEach { it.stop() }
        return statuses()
    }

    fun statuses(): List<CarrierRuntimeStatus> = runners.map { it.status() }
}

private class CarrierRunner(
    private val plan: CarrierRuntimePlan,
    private val hooks: AndroidPlatformHooks,
    private val transportFactory: CarrierTransportFactory,
    private val initialBackoffMs: Long,
    private val maxBackoffMs: Long,
) {
    private var state = CarrierRuntimeState.STOPPED
    private var transport: CarrierTransport? = null
    private var attempts = 0
    private var successfulProbes = 0
    private var reconnects = 0
    private var lastError: String? = null
    private var backoffMs = initialBackoffMs
    private var nextRetryAtMs = 0L
    private var nextRetryDelayMs = 0L
    private var nextProbeAtMs = Long.MAX_VALUE

    fun start(nowMs: Long) {
        if (state != CarrierRuntimeState.STOPPED) {
            return
        }
        state = CarrierRuntimeState.RESOLVING
        resolveAndConnect(nowMs)
    }

    fun tick(nowMs: Long) {
        when (state) {
            CarrierRuntimeState.RUNNING -> {
                if (nowMs >= nextProbeAtMs) {
                    probe(nowMs)
                }
            }
            CarrierRuntimeState.BACKOFF -> {
                if (nowMs >= nextRetryAtMs) {
                    state = CarrierRuntimeState.RESOLVING
                    resolveAndConnect(nowMs)
                }
            }
            CarrierRuntimeState.RESOLVING,
            CarrierRuntimeState.PROTECTING,
            CarrierRuntimeState.CONNECTING,
            CarrierRuntimeState.STOPPED,
            CarrierRuntimeState.FAILED,
            -> Unit
        }
    }

    fun stop() {
        transport?.close()
        transport = null
        state = CarrierRuntimeState.STOPPED
        nextRetryDelayMs = 0
        nextRetryAtMs = 0
        nextProbeAtMs = Long.MAX_VALUE
    }

    fun status() = CarrierRuntimeStatus(
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
        state = CarrierRuntimeState.RESOLVING
        val endpoints = hooks.resolveOnUnderlyingNetwork(plan.probe.endpoint.host, plan.probe.endpoint.port)
        if (endpoints.isEmpty()) {
            enterBackoff("resolve_empty", nowMs)
            return
        }

        val candidate = transportFactory.create(plan)
        transport = candidate

        state = CarrierRuntimeState.PROTECTING
        if (!hooks.protectSocket(candidate.socketFd)) {
            candidate.close()
            transport = null
            enterBackoff("socket_protect_failed", nowMs)
            return
        }

        state = CarrierRuntimeState.CONNECTING
        attempts += 1
        val connected = candidate.connect(endpoints.first())
        if (!connected.ok) {
            candidate.close()
            transport = null
            enterBackoff(connected.error ?: "connect_failed", nowMs)
            return
        }

        state = CarrierRuntimeState.RUNNING
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
        state = CarrierRuntimeState.BACKOFF
        lastError = error
        nextRetryDelayMs = backoffMs
        nextRetryAtMs = nowMs + backoffMs
        backoffMs = (backoffMs * 2).coerceAtMost(maxBackoffMs)
    }
}
