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

    fun startFakeCarrier(handle: Long, rejectFrames: Boolean): NativeRuntimeSnapshot {
        return nativeStartFakeCarrierForTest(handle, rejectFrames)
    }

    fun stopFakeCarrier(handle: Long): NativeRuntimeSnapshot {
        return nativeStopFakeCarrierForTest(handle)
    }

    fun capturedFakeCarrierFrameDigests(handle: Long): List<String> {
        return nativeCapturedFakeCarrierFrameDigestsForTest(handle).toList()
    }

    fun runZeroRttServerPeer(fd: Int, profileId: String, clientUuid: String, tamperServerAccept: Boolean): String {
        return nativeRunZeroRttServerPeerForTest(fd, profileId, clientUuid, tamperServerAccept)
    }

    private external fun nativeInstallTunPacketCaptureSinkForTest(handle: Long, rejectPackets: Boolean): NativeRuntimeSnapshot

    private external fun nativeCapturedTunPacketDigestsForTest(handle: Long): Array<String>

    private external fun nativeStartFakeCarrierForTest(handle: Long, rejectFrames: Boolean): NativeRuntimeSnapshot

    private external fun nativeStopFakeCarrierForTest(handle: Long): NativeRuntimeSnapshot

    private external fun nativeCapturedFakeCarrierFrameDigestsForTest(handle: Long): Array<String>

    private external fun nativeRunZeroRttServerPeerForTest(fd: Int, profileId: String, clientUuid: String, tamperServerAccept: Boolean): String
}
