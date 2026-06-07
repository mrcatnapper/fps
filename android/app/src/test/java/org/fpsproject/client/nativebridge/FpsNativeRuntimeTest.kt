package org.fpsproject.client.nativebridge

import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.TunHandle
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Base64

class FpsNativeRuntimeTest {
    private val uuid = "123e4567-e89b-42d3-a456-426614174000"
    private val key = Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })
    private val profileJson = """
        {
          "network": {"server": "fps.example.test:443"},
          "security": {
            "zero_rtt": {
              "enabled": true,
              "profile_id": "android-test-v5",
              "client_uuid": "$uuid",
              "server_public_key_base64": "$key"
            }
          },
          "tun": {"enabled": true, "mtu": 1280}
        }
    """.trimIndent()

    @Test
    fun createValidatesProfileBeforeNativeHandleCreation() {
        val backend = FakeNativeBackend()

        assertThrows(IllegalArgumentException::class.java) {
            FpsNativeRuntime.create("""{"network": {"server": "fps.example.test:443"}}""", backend)
        }

        assertEquals(0, backend.createdProfiles.size)
    }

    @Test
    fun createSnapshotAndCloseUseOpaqueHandle() {
        val backend = FakeNativeBackend()
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val snapshot = runtime.snapshot()

        assertTrue(snapshot.alive)
        assertFalse(snapshot.tunAttached)
        assertEquals(1L, backend.createdProfiles.single().first)
        assertEquals(profileJson, backend.createdProfiles.single().second)
        assertFalse(snapshot.toString().contains(uuid))
        assertFalse(snapshot.toString().contains(key))

        runtime.close()
        runtime.close()

        assertEquals(listOf(1L), backend.closedHandles)
        assertEquals("runtime_closed", runtime.snapshot().lastError)
    }

    @Test
    fun attachTunPassesBorrowedFdWithoutOwningKotlinHandle() {
        val backend = FakeNativeBackend()
        val handle = FakeTunHandle(fd = 77)
        val runtime = FpsNativeRuntime.create(profileJson, backend)

        val snapshot = runtime.attachTun(EstablishedTun.owned(fd = handle.fd, mtu = 1280, handle = handle))
        runtime.close()

        assertTrue(snapshot.tunAttached)
        assertEquals(77, snapshot.tunFd)
        assertEquals(1280, snapshot.tunMtu)
        assertEquals(0, handle.closeCount)
        assertEquals(listOf(Triple(1L, 77, 1280)), backend.attachedTun)
    }

    @Test
    fun attachAfterCloseReportsClosedRuntime() {
        val runtime = FpsNativeRuntime.create(profileJson, FakeNativeBackend())

        runtime.close()
        val snapshot = runtime.attachTunFd(77, 1280)

        assertFalse(snapshot.alive)
        assertEquals("runtime_closed", snapshot.lastError)
    }
}

private class FakeNativeBackend : FpsNativeBackend {
    private var nextHandle = 1L
    val createdProfiles = mutableListOf<Pair<Long, String>>()
    val closedHandles = mutableListOf<Long>()
    val attachedTun = mutableListOf<Triple<Long, Int, Int>>()
    private val snapshots = mutableMapOf<Long, NativeRuntimeSnapshot>()

    override fun createRuntime(profileText: String): Long {
        val handle = nextHandle++
        createdProfiles += handle to profileText
        snapshots[handle] = NativeRuntimeSnapshot(
            alive = true,
            tunAttached = false,
            tunFd = -1,
            tunMtu = 0,
            lastError = null,
        )
        return handle
    }

    override fun closeRuntime(handle: Long) {
        closedHandles += handle
        snapshots.remove(handle)
    }

    override fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot {
        return snapshots[handle] ?: NativeRuntimeSnapshot(
            alive = false,
            tunAttached = false,
            tunFd = -1,
            tunMtu = 0,
            lastError = "invalid_handle",
        )
    }

    override fun attachTunFd(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot {
        attachedTun += Triple(handle, fd, mtu)
        val snapshot = NativeRuntimeSnapshot(
            alive = true,
            tunAttached = true,
            tunFd = fd,
            tunMtu = mtu,
            lastError = null,
        )
        snapshots[handle] = snapshot
        return snapshot
    }
}

private class FakeTunHandle(override val fd: Int) : TunHandle {
    var closeCount = 0

    override fun close() {
        closeCount += 1
    }
}
