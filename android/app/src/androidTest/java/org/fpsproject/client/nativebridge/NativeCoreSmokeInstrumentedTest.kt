package org.fpsproject.client.nativebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.fpsproject.client.policy.TunProtocol

class NativeCoreSmokeInstrumentedTest {
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
}
