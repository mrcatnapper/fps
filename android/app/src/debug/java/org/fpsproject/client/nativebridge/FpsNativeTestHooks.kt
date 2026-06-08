package org.fpsproject.client.nativebridge

object FpsNativeTestHooks {
    init {
        System.loadLibrary("fps_android_native")
    }

    fun installTunPacketCaptureSink(handle: Long, rejectPackets: Boolean): NativeRuntimeSnapshot {
        return nativeInstallTunPacketCaptureSinkForTest(handle, rejectPackets)
    }

    fun capturedTunPacketDigests(handle: Long): List<String> {
        return nativeCapturedTunPacketDigestsForTest(handle).toList()
    }

    private external fun nativeInstallTunPacketCaptureSinkForTest(handle: Long, rejectPackets: Boolean): NativeRuntimeSnapshot

    private external fun nativeCapturedTunPacketDigestsForTest(handle: Long): Array<String>
}
