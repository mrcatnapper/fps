package org.fpsproject.client

import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerState
import org.fpsproject.client.nativebridge.NativeVpnRuntimeSnapshot
import org.fpsproject.client.runtime.VpnRuntimeState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FpsVpnServiceRuntimeTest {
    @Test
    fun startProfileCreatesAndStartsRunner() {
        val factory = FakeServiceRunnerFactory()
        val runtime = FpsVpnServiceRuntime(factory)

        val state = runtime.startProfile(PROFILE_A)

        assertEquals(CoordinatedNativeVpnRunnerState.RUNNING, state)
        assertEquals(listOf(PROFILE_A), factory.createdProfiles)
        assertEquals(1, factory.createdRunners.single().startCount)
        assertEquals(CoordinatedNativeVpnRunnerState.RUNNING, runtime.runnerSnapshot().state)
    }

    @Test
    fun restartClosesPreviousRunnerAndUsesNewRunnerSnapshot() {
        val factory = FakeServiceRunnerFactory()
        val runtime = FpsVpnServiceRuntime(factory)

        runtime.startProfile(PROFILE_A)
        val first = factory.createdRunners.single()
        runtime.startProfile(PROFILE_B)
        val second = factory.createdRunners.last()

        assertEquals(listOf(PROFILE_A, PROFILE_B), factory.createdProfiles)
        assertEquals(1, first.closeCount)
        assertEquals(1, second.startCount)
        assertEquals(0, second.closeCount)
        assertEquals(second.snapshot(), runtime.runnerSnapshot())
    }

    @Test
    fun factoryFailureKeepsPreviousRunnerAlive() {
        val factory = FakeServiceRunnerFactory()
        val runtime = FpsVpnServiceRuntime(factory)

        runtime.startProfile(PROFILE_A)
        val first = factory.createdRunners.single()
        factory.nextFailure = IllegalArgumentException("bad profile")

        val thrown = runCatching { runtime.startProfile(PROFILE_B) }.exceptionOrNull()

        assertTrue(thrown is IllegalArgumentException)
        assertEquals(0, first.closeCount)
        assertEquals(first.snapshot(), runtime.runnerSnapshot())
        assertEquals(listOf(PROFILE_A, PROFILE_B), factory.createdProfiles)
    }

    @Test
    fun stopIsIdempotentAndReturnsStoppedSnapshot() {
        val factory = FakeServiceRunnerFactory()
        val runtime = FpsVpnServiceRuntime(factory)

        runtime.startProfile(PROFILE_A)
        val runner = factory.createdRunners.single()

        assertEquals(VpnRuntimeState.STOPPED, runtime.stop())
        assertEquals(VpnRuntimeState.STOPPED, runtime.stop())

        assertEquals(1, runner.closeCount)
        assertEquals(CoordinatedNativeVpnRunnerState.STOPPED, runtime.runnerSnapshot().state)
        assertEquals(VpnRuntimeState.STOPPED, runtime.nativeSnapshot().vpn.state)
    }

    @Test
    fun snapshotsDoNotExposeProfileTextOrIdentityMaterial() {
        val factory = FakeServiceRunnerFactory()
        val runtime = FpsVpnServiceRuntime(factory)

        runtime.startProfile(PROFILE_A)
        val text = runtime.runnerSnapshot().toString() + runtime.nativeSnapshot().toString()

        assertFalse(text.contains(PROFILE_A))
        assertFalse(text.contains(UUID_A))
        assertFalse(text.contains(SERVER_KEY_A))
    }

    private companion object {
        private const val UUID_A = "123e4567-e89b-42d3-a456-426614174000"
        private const val SERVER_KEY_A = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
        private val PROFILE_A = """
            {"security":{"zero_rtt":{"client_uuid":"$UUID_A","server_public_key_base64":"$SERVER_KEY_A"}}}
        """.trimIndent()
        private val PROFILE_B = """
            {"security":{"zero_rtt":{"client_uuid":"123e4567-e89b-42d3-a456-426614174001"}}}
        """.trimIndent()
    }
}

private class FakeServiceRunnerFactory : FpsVpnServiceRunnerFactory {
    val createdProfiles = mutableListOf<String>()
    val createdRunners = mutableListOf<FakeServiceRunner>()
    var nextFailure: RuntimeException? = null

    override fun create(profileText: String): FpsVpnServiceRunner {
        createdProfiles += profileText
        nextFailure?.let {
            nextFailure = null
            throw it
        }
        return FakeServiceRunner().also { createdRunners += it }
    }
}

private class FakeServiceRunner : FpsVpnServiceRunner {
    var startCount = 0
    var closeCount = 0
    private var state = CoordinatedNativeVpnRunnerState.STOPPED

    override fun start(): CoordinatedNativeVpnRunnerSnapshot {
        startCount += 1
        state = CoordinatedNativeVpnRunnerState.RUNNING
        return snapshot()
    }

    override fun snapshot() = CoordinatedNativeVpnRunnerSnapshot(
        state = state,
        attempts = startCount,
        reconnects = 0,
        lastError = null,
        nextRetryDelayMs = 0,
        runtime = NativeVpnRuntimeSnapshot.stopped(),
    )

    override fun close() {
        closeCount += 1
        state = CoordinatedNativeVpnRunnerState.STOPPED
    }
}
