package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.AndroidClientProfileParser
import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.policy.TunFlowTuple

const val TUN_FD_OWNERSHIP_OWNED_DUPLICATE = "owned_duplicate"

data class NativeRuntimeSnapshot(
    val alive: Boolean,
    val tunAttached: Boolean,
    val tunFd: Int,
    val tunMtu: Int,
    val tunFdOwnership: String?,
    val lastError: String?,
)

interface FpsNativeBackend {
    fun createRuntime(profileText: String): Long

    fun closeRuntime(handle: Long)

    fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot

    fun attachTunFdOwnedDuplicate(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot
}

class FpsNativeRuntime private constructor(
    private var handle: Long,
    private val backend: FpsNativeBackend,
) : AutoCloseable {
    companion object {
        fun create(profileText: String, backend: FpsNativeBackend = FpsNative): FpsNativeRuntime {
            val profile = AndroidClientProfileParser.parse(profileText)
            return createForValidatedProfile(profileText, profile, backend)
        }

        internal fun createForValidatedProfile(
            profileText: String,
            @Suppress("UNUSED_PARAMETER") profile: AndroidClientProfile,
            backend: FpsNativeBackend = FpsNative,
        ): FpsNativeRuntime {
            val handle = backend.createRuntime(profileText)
            require(handle != 0L) { "native runtime creation failed" }
            return FpsNativeRuntime(handle, backend)
        }
    }

    fun snapshot(): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return NativeRuntimeSnapshot(
                alive = false,
                tunAttached = false,
                tunFd = -1,
                tunMtu = 0,
                tunFdOwnership = null,
                lastError = "runtime_closed",
            )
        }
        return backend.runtimeSnapshot(activeHandle)
    }

    fun attachTun(tun: EstablishedTun): NativeRuntimeSnapshot = attachTunFdOwnedDuplicate(tun.fd, tun.mtu)

    fun attachTunFdOwnedDuplicate(fd: Int, mtu: Int): NativeRuntimeSnapshot {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return snapshot()
        }
        return backend.attachTunFdOwnedDuplicate(activeHandle, fd, mtu)
    }

    override fun close() {
        val activeHandle = handle
        if (activeHandle == 0L) {
            return
        }
        handle = 0
        backend.closeRuntime(activeHandle)
    }
}

object FpsNative : FpsNativeBackend {
    init {
        System.loadLibrary("fps_android_native")
    }

    external fun nativeVersion(): String

    external fun nativeCoreSmoke(): String

    external fun parseIpv4FlowTuple(packet: ByteArray): TunFlowTuple?

    external override fun createRuntime(profileText: String): Long

    external override fun closeRuntime(handle: Long)

    external override fun runtimeSnapshot(handle: Long): NativeRuntimeSnapshot

    external override fun attachTunFdOwnedDuplicate(handle: Long, fd: Int, mtu: Int): NativeRuntimeSnapshot
}
