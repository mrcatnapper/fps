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
) {
    private var runner: FpsVpnServiceRunner? = null

    fun startProfile(profileText: String): CoordinatedNativeVpnRunnerState {
        val next = runnerFactory.create(profileText)
        runner?.close()
        runner = next
        next.start()
        return next.snapshot().state
    }

    fun stop(): VpnRuntimeState {
        val active = runner ?: return VpnRuntimeState.STOPPED
        runner = null
        active.close()
        return VpnRuntimeState.STOPPED
    }

    fun runnerSnapshot(): CoordinatedNativeVpnRunnerSnapshot {
        return runner?.snapshot() ?: CoordinatedNativeVpnRunnerSnapshot.stopped()
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
