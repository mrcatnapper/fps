package org.fpsproject.client.nativebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.fpsproject.client.policy.TunProtocol
import java.util.Base64

class NativeCoreSmokeInstrumentedTest {
    private val profileJson = """
        {
          "network": {"server": "fps.example.test:443"},
          "security": {
            "zero_rtt": {
              "enabled": true,
              "profile_id": "android-test-v5",
              "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
              "server_public_key_base64": "${Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })}"
            }
          },
          "tun": {"enabled": true, "mtu": 1280}
        }
    """.trimIndent()

    @Test
    fun loadsNativeLibraryAndRunsCoreSmoke() {
        assertTrue(FpsNative.nativeVersion().startsWith("fps-android-native/"))
        assertEquals("ok", FpsNative.nativeCoreSmoke())
    }

    @Test
    fun parsesIpv4TcpFlowTuple() {
        val packet = byteArrayOf(
            0x45, 0x00, 0x00, 0x28,
            0x00, 0x00, 0x40, 0x00,
            0x40, 0x06, 0x00, 0x00,
            0x0a, 0x42, 0x00, 0x02,
            0x5d.toByte(), 0xb8.toByte(), 0xd8.toByte(), 0x22,
            0xcf.toByte(), 0x08, 0x01, 0xbb.toByte(),
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x50, 0x02, 0x20, 0x00,
            0x00, 0x00, 0x00, 0x00,
        )

        val tuple = FpsNative.parseIpv4FlowTuple(packet)!!

        assertEquals(TunProtocol.TCP, tuple.protocol)
        assertEquals(0x0a420002L, tuple.sourceIpv4)
        assertEquals(53000, tuple.sourcePort)
        assertEquals(0x5db8d822L, tuple.destinationIpv4)
        assertEquals(443, tuple.destinationPort)
    }

    @Test
    fun nativeRuntimeHandleLifecycleAndBorrowedTunSnapshot() {
        val handle = FpsNative.createRuntime(profileJson)
        assertTrue(handle != 0L)

        val initial = FpsNative.runtimeSnapshot(handle)
        assertTrue(initial.alive)
        assertFalse(initial.tunAttached)
        assertEquals(null, initial.tunFdOwnership)
        assertEquals(null, initial.lastError)

        val attached = FpsNative.attachTunFd(handle, 123, 1280)
        assertTrue(attached.alive)
        assertTrue(attached.tunAttached)
        assertEquals(123, attached.tunFd)
        assertEquals(1280, attached.tunMtu)
        assertEquals("borrowed", attached.tunFdOwnership)

        val badFd = FpsNative.attachTunFd(handle, -1, 1280)
        assertTrue(badFd.alive)
        assertFalse(badFd.tunAttached)
        assertEquals(null, badFd.tunFdOwnership)
        assertEquals("invalid_tun_fd", badFd.lastError)

        val badMtu = FpsNative.attachTunFd(handle, 123, 0)
        assertTrue(badMtu.alive)
        assertFalse(badMtu.tunAttached)
        assertEquals(null, badMtu.tunFdOwnership)
        assertEquals("invalid_tun_mtu", badMtu.lastError)

        FpsNative.closeRuntime(handle)
        val closed = FpsNative.runtimeSnapshot(handle)
        assertFalse(closed.alive)
        assertEquals(null, closed.tunFdOwnership)
        assertEquals("invalid_handle", closed.lastError)
    }
}
