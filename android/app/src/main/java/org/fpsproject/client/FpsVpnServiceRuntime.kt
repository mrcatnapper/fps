package org.fpsproject.client

import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunner
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerState
import org.fpsproject.client.nativebridge.NativeVpnRuntimeSnapshot
import org.fpsproject.client.runtime.VpnRuntimeState

internal interface FpsVpnServiceRunner : AutoCloseable {
    fun start(): CoordinatedNativeVpnRunnerSnapshot

    fun snapshot(): CoordinatedNativeVpnRunnerSnapshot
}

internal fun interface FpsVpnServiceRunnerFactory {
    fun create(profileText: String): FpsVpnServiceRunner
}

internal class FpsVpnServiceRuntime(
    private val runnerFactory: FpsVpnServiceRunnerFactory,
    private val statusNotifier: FpsVpnStatusNotifier = NoopFpsVpnStatusNotifier,
) {
    private var runner: FpsVpnServiceRunner? = null

    fun startProfile(profileText: String): CoordinatedNativeVpnRunnerState {
        val next = runnerFactory.create(profileText)
        statusNotifier.show(FpsVpnStatusSnapshot.starting())
        runner?.close()
        runner = next
        next.start()
        val snapshot = next.snapshot()
        statusNotifier.show(FpsVpnStatusSnapshot.fromRunner(snapshot))
        return snapshot.state
    }

    fun stop(): VpnRuntimeState {
        val active = runner
        if (active == null) {
            statusNotifier.clear()
            return VpnRuntimeState.STOPPED
        }
        runner = null
        active.close()
        statusNotifier.clear()
        return VpnRuntimeState.STOPPED
    }

    fun runnerSnapshot(): CoordinatedNativeVpnRunnerSnapshot {
        val snapshot = runner?.snapshot() ?: CoordinatedNativeVpnRunnerSnapshot.stopped()
        if (runner != null) {
            statusNotifier.show(FpsVpnStatusSnapshot.fromRunner(snapshot))
        }
        return snapshot
    }

    fun nativeSnapshot(): NativeVpnRuntimeSnapshot {
        return runnerSnapshot().runtime
    }
}

internal class CoordinatedNativeVpnServiceRunner(
    private val runner: CoordinatedNativeVpnRunner,
) : FpsVpnServiceRunner {
    override fun start(): CoordinatedNativeVpnRunnerSnapshot = runner.start()

    override fun snapshot(): CoordinatedNativeVpnRunnerSnapshot = runner.snapshot()

    override fun close() {
        runner.close()
    }
}
