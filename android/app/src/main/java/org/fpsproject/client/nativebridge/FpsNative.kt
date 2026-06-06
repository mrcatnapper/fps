package org.fpsproject.client.nativebridge

import org.fpsproject.client.policy.TunFlowTuple

object FpsNative {
    init {
        System.loadLibrary("fps_android_native")
    }

    external fun nativeVersion(): String

    external fun nativeCoreSmoke(): String

    external fun parseIpv4FlowTuple(packet: ByteArray): TunFlowTuple?
}
