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
        const val ACTION_CLEAR_PROFILE = "org.fpsproject.client.action.CLEAR_PROFILE"
        const val EXTRA_PROFILE = "org.fpsproject.client.extra.PROFILE"
    }

    private val serviceRuntime by lazy {
        FpsVpnServiceRuntime(
            runnerFactory = { profileText ->
                val hooks = VpnServicePlatformHooks(this)
                CoordinatedNativeVpnServiceRunner(
                    CoordinatedNativeVpnRunner(
                        runtimeFactory = { HeadlessNativeVpnRuntime.create(profileText, hooks) },
                        coverClientStarter = RawHttpsLocalCoverClientStarter(),
                    ),
                )
            },
            statusNotifier = AndroidFpsVpnStatusNotifier(this),
            profileRepository = SharedPreferencesFpsVpnProfileRepository(this),
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopRuntime()
                stopSelf(startId)
                return START_NOT_STICKY
            }
            ACTION_START, null -> {
                val profileText = intent?.getStringExtra(EXTRA_PROFILE)
                val state = if (profileText != null) {
                    startProfile(profileText)
                } else {
                    startSavedProfile()
                }
                return if (state == CoordinatedNativeVpnRunnerState.FAILED) START_NOT_STICKY else START_STICKY
            }
            ACTION_CLEAR_PROFILE -> {
                stopRuntime()
                serviceRuntime.clearProfile()
                stopSelf(startId)
                return START_NOT_STICKY
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
        return serviceRuntime.saveAndStartProfile(profileText)
    }

    internal fun startSavedProfile(): CoordinatedNativeVpnRunnerState {
        return serviceRuntime.startSavedProfile()
    }

    internal fun stopRuntime(): VpnRuntimeState {
        return serviceRuntime.stop()
    }

    internal fun runnerSnapshot(): CoordinatedNativeVpnRunnerSnapshot {
        return serviceRuntime.runnerSnapshot()
    }

    internal fun snapshot(): NativeVpnRuntimeSnapshot {
        return serviceRuntime.nativeSnapshot()
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
