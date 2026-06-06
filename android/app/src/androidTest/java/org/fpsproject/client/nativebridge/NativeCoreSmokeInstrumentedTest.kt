package org.fpsproject.client.nativebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeCoreSmokeInstrumentedTest {
    @Test
    fun loadsNativeLibraryAndRunsCoreSmoke() {
        assertTrue(FpsNative.nativeVersion().startsWith("fps-android-native/"))
        assertEquals("ok", FpsNative.nativeCoreSmoke())
    }
}
