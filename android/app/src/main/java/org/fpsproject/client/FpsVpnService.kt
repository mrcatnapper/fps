package org.fpsproject.client

import android.content.Intent
import android.net.VpnService
import android.os.ParcelFileDescriptor
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunner
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerState
import org.fpsproject.client.nativebridge.HeadlessNativeVpnRuntime
import org.fpsproject.client.nativebridge.NativeVpnRuntimeSnapshot
import org.fpsproject.client.nativebridge.RawHttpsLocalCoverClientStarter
import org.fpsproject.client.runtime.TunHandle
import org.fpsproject.client.runtime.VpnRuntimeState

class FpsVpnService : VpnService() {
    companion object {
        const val ACTION_START = "org.fpsproject.client.action.START"
        const val ACTION_STOP = "org.fpsproject.client.action.STOP"
        const val EXTRA_PROFILE = "org.fpsproject.client.extra.PROFILE"
    }

    private var runner: CoordinatedNativeVpnRunner? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopRuntime()
                stopSelf(startId)
                return START_NOT_STICKY
            }
            ACTION_START, null -> {
                val profileText = intent?.getStringExtra(EXTRA_PROFILE)
                if (profileText != null) {
                    startProfile(profileText)
                }
                return START_STICKY
            }
            else -> return START_NOT_STICKY
        }
    }

    override fun onDestroy() {
        stopRuntime()
        super.onDestroy()
    }

    override fun onRevoke() {
        stopRuntime()
        super.onRevoke()
    }

    internal fun startProfile(profileText: String): CoordinatedNativeVpnRunnerState {
        val hooks = VpnServicePlatformHooks(this)
        val next = CoordinatedNativeVpnRunner(
            runtimeFactory = { HeadlessNativeVpnRuntime.create(profileText, hooks) },
            coverClientStarter = RawHttpsLocalCoverClientStarter(),
        )
        runner?.close()
        runner = next
        next.start()
        return next.snapshot().state
    }

    internal fun stopRuntime(): VpnRuntimeState {
        val activeRunner = runner ?: return VpnRuntimeState.STOPPED
        activeRunner.close()
        runner = null
        return VpnRuntimeState.STOPPED
    }

    internal fun runnerSnapshot(): CoordinatedNativeVpnRunnerSnapshot {
        return runner?.snapshot() ?: CoordinatedNativeVpnRunnerSnapshot.stopped()
    }

    internal fun snapshot(): NativeVpnRuntimeSnapshot {
        return runnerSnapshot().runtime
    }
}

class AndroidVpnTunnelBuilder(
    service: VpnService,
) : org.fpsproject.client.runtime.VpnTunnelBuilder {
    private val builder = service.Builder()

    override fun setSession(name: String) {
        builder.setSession(name)
    }

    override fun setMtu(mtu: Int) {
        builder.setMtu(mtu)
    }

    override fun addAddress(address: String, prefixLength: Int) {
        builder.addAddress(address, prefixLength)
    }

    override fun addRoute(address: String, prefixLength: Int) {
        builder.addRoute(address, prefixLength)
    }

    override fun addDnsServer(address: String) {
        builder.addDnsServer(address)
    }

    override fun establish(): TunHandle? {
        return builder.establish()?.let { ParcelTunHandle(it) }
    }
}

private class ParcelTunHandle(
    private val descriptor: ParcelFileDescriptor,
) : TunHandle {
    override val fd: Int
        get() = descriptor.fd

    override fun close() {
        descriptor.close()
    }
}
