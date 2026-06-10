package org.fpsproject.client.nativebridge

import org.fpsproject.client.runtime.VpnRuntimeState
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

enum class CoordinatedNativeVpnRunnerState {
    STOPPED,
    STARTING,
    WAITING_FOR_LEASE,
    RUNNING,
    BACKOFF,
    NEEDS_VPN_PERMISSION,
    FAILED,
}

data class CoordinatedNativeVpnRunnerSnapshot(
    val state: CoordinatedNativeVpnRunnerState,
    val attempts: Int,
    val reconnects: Int,
    val lastError: String?,
    val nextRetryDelayMs: Long,
    val runtime: NativeVpnRuntimeSnapshot,
) {
    companion object {
        fun stopped() = CoordinatedNativeVpnRunnerSnapshot(
            state = CoordinatedNativeVpnRunnerState.STOPPED,
            attempts = 0,
            reconnects = 0,
            lastError = null,
            nextRetryDelayMs = 0,
            runtime = NativeVpnRuntimeSnapshot.stopped(),
        )
    }
}

fun interface CoordinatedNativeVpnScheduledTask {
    fun cancel()
}

interface CoordinatedNativeVpnRunnerScheduler : AutoCloseable {
    fun nowMs(): Long

    fun schedule(delayMs: Long, task: () -> Unit): CoordinatedNativeVpnScheduledTask
}

class ExecutorCoordinatedNativeVpnRunnerScheduler(
    threadName: String = "fps-android-native-runner",
) : CoordinatedNativeVpnRunnerScheduler {
    @Volatile
    private var closed = false

    private val executor = Executors.newSingleThreadScheduledExecutor { task ->
        Thread(task, threadName).also { it.isDaemon = true }
    }

    override fun nowMs(): Long = System.currentTimeMillis()

    override fun schedule(delayMs: Long, task: () -> Unit): CoordinatedNativeVpnScheduledTask {
        if (closed) {
            return CoordinatedNativeVpnScheduledTask {}
        }
        val future = executor.schedule(
            {
                if (!closed) {
                    task()
                }
            },
            delayMs.coerceAtLeast(0),
            TimeUnit.MILLISECONDS,
        )
        return FutureScheduledTask(future)
    }

    override fun close() {
        closed = true
        executor.shutdownNow()
    }
}

private class FutureScheduledTask(
    private val future: ScheduledFuture<*>,
) : CoordinatedNativeVpnScheduledTask {
    override fun cancel() {
        future.cancel(false)
    }
}

class CoordinatedNativeVpnRunner(
    private val runtimeFactory: () -> HeadlessNativeVpnRuntime,
    private val coverClientStarter: LocalCoverClientStarter,
    private val scheduler: CoordinatedNativeVpnRunnerScheduler = ExecutorCoordinatedNativeVpnRunnerScheduler(),
    private val initialBackoffMs: Long = 1000,
    private val maxBackoffMs: Long = 30_000,
    private val tickIntervalMs: Long = 250,
    private val maxNativeEvents: Int = 16,
    private val maxPolicyPackets: Int = 64,
) : AutoCloseable {
    private var state = CoordinatedNativeVpnRunnerState.STOPPED
    private var attempts = 0
    private var reconnects = 0
    private var lastError: String? = null
    private var nextRetryDelayMs = 0L
    private var backoffMs = initialBackoffMs
    private var runtime: HeadlessNativeVpnRuntime? = null
    private var scheduledTask: CoordinatedNativeVpnScheduledTask? = null
    private var lastRuntimeSnapshot = NativeVpnRuntimeSnapshot.stopped()

    @Synchronized
    fun start(): CoordinatedNativeVpnRunnerSnapshot {
        if (state != CoordinatedNativeVpnRunnerState.STOPPED) {
            return snapshotLocked()
        }
        state = CoordinatedNativeVpnRunnerState.STARTING
        attempts = 0
        reconnects = 0
        lastError = null
        nextRetryDelayMs = 0
        backoffMs = initialBackoffMs
        scheduleLocked(delayMs = 0) { runStartAttempt() }
        return snapshotLocked()
    }

    @Synchronized
    fun tick(): CoordinatedNativeVpnRunnerSnapshot {
        runTick()
        return snapshotLocked()
    }

    @Synchronized
    fun stop(): CoordinatedNativeVpnRunnerSnapshot {
        cancelScheduledLocked()
        closeRuntimeLocked()
        state = CoordinatedNativeVpnRunnerState.STOPPED
        lastError = null
        nextRetryDelayMs = 0
        lastRuntimeSnapshot = NativeVpnRuntimeSnapshot.stopped()
        return snapshotLocked()
    }

    @Synchronized
    fun snapshot(): CoordinatedNativeVpnRunnerSnapshot = snapshotLocked()

    @Synchronized
    override fun close() {
        stop()
        scheduler.close()
    }

    @Synchronized
    private fun runStartAttempt() {
        scheduledTask = null
        if (state == CoordinatedNativeVpnRunnerState.STOPPED) {
            return
        }
        closeRuntimeLocked()
        state = CoordinatedNativeVpnRunnerState.STARTING
        attempts += 1
        val activeRuntime = try {
            runtimeFactory()
        } catch (_: RuntimeException) {
            state = CoordinatedNativeVpnRunnerState.FAILED
            lastError = "runtime_create_failed"
            nextRetryDelayMs = 0
            return
        }
        runtime = activeRuntime
        val result = try {
            activeRuntime.startCoordinated(
                coverClientStarter = coverClientStarter,
                maxNativeEvents = maxNativeEvents,
                maxPolicyPackets = maxPolicyPackets,
            )
        } catch (_: RuntimeException) {
            enterBackoffLocked("coordinated_start_failed")
            return
        }
        lastRuntimeSnapshot = result
        updateAfterRuntimeSnapshotLocked(result)
    }

    @Synchronized
    private fun runTick() {
        if (state != CoordinatedNativeVpnRunnerState.WAITING_FOR_LEASE &&
            state != CoordinatedNativeVpnRunnerState.RUNNING
        ) {
            return
        }
        val activeRuntime = runtime
        if (activeRuntime == null) {
            enterBackoffLocked("runtime_missing")
            return
        }
        val result = try {
            activeRuntime.tickCoordinated(
                maxNativeEvents = maxNativeEvents,
                maxPolicyPackets = maxPolicyPackets,
            )
        } catch (_: RuntimeException) {
            enterBackoffLocked("coordinated_tick_failed")
            return
        }
        lastRuntimeSnapshot = result
        updateAfterRuntimeSnapshotLocked(result)
    }

    private fun updateAfterRuntimeSnapshotLocked(result: NativeVpnRuntimeSnapshot) {
        when (result.vpn.state) {
            VpnRuntimeState.RUNNING -> {
                state = CoordinatedNativeVpnRunnerState.RUNNING
                lastError = null
                nextRetryDelayMs = 0
                backoffMs = initialBackoffMs
                scheduleLocked(tickIntervalMs) { runTick() }
            }
            VpnRuntimeState.WAITING_FOR_LEASE -> {
                state = CoordinatedNativeVpnRunnerState.WAITING_FOR_LEASE
                lastError = null
                nextRetryDelayMs = 0
                scheduleLocked(tickIntervalMs) { runTick() }
            }
            VpnRuntimeState.NEEDS_VPN_PERMISSION -> {
                closeRuntimeLocked()
                state = CoordinatedNativeVpnRunnerState.NEEDS_VPN_PERMISSION
                lastError = result.vpn.lastError
                nextRetryDelayMs = 0
            }
            VpnRuntimeState.FAILED -> {
                val error = result.vpn.lastError ?: result.native.lastError ?: "coordinated_runtime_failed"
                if (error == "carrier_profile_missing") {
                    closeRuntimeLocked()
                    state = CoordinatedNativeVpnRunnerState.FAILED
                    lastError = error
                    nextRetryDelayMs = 0
                    return
                }
                enterBackoffLocked(error)
            }
            VpnRuntimeState.STOPPED -> {
                enterBackoffLocked("coordinated_runtime_stopped")
            }
        }
    }

    private fun enterBackoffLocked(error: String) {
        closeRuntimeLocked()
        state = CoordinatedNativeVpnRunnerState.BACKOFF
        lastError = error
        reconnects += 1
        nextRetryDelayMs = backoffMs
        scheduleLocked(backoffMs) { runStartAttempt() }
        backoffMs = (backoffMs * 2).coerceAtMost(maxBackoffMs)
    }

    private fun scheduleLocked(delayMs: Long, task: () -> Unit) {
        cancelScheduledLocked()
        scheduledTask = scheduler.schedule(delayMs, task)
    }

    private fun cancelScheduledLocked() {
        scheduledTask?.cancel()
        scheduledTask = null
    }

    private fun closeRuntimeLocked() {
        val activeRuntime = runtime ?: return
        runtime = null
        activeRuntime.close()
    }

    private fun snapshotLocked() = CoordinatedNativeVpnRunnerSnapshot(
        state = state,
        attempts = attempts,
        reconnects = reconnects,
        lastError = lastError,
        nextRetryDelayMs = nextRetryDelayMs,
        runtime = runtime?.snapshot() ?: lastRuntimeSnapshot,
    )
}
