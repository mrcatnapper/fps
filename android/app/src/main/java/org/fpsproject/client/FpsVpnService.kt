package org.fpsproject.client

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.VpnService
import android.os.ParcelFileDescriptor
import android.system.OsConstants
import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.nativebridge.HeadlessNativeVpnRuntime
import org.fpsproject.client.nativebridge.NativeVpnRuntimeSnapshot
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.TunProtocol
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.ResolvedEndpoint
import org.fpsproject.client.runtime.TunHandle
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnRuntimeState
import org.fpsproject.client.runtime.VpnTunEstablisher
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket

class FpsVpnService : VpnService() {
    companion object {
        const val ACTION_START = "org.fpsproject.client.action.START"
        const val ACTION_STOP = "org.fpsproject.client.action.STOP"
        const val EXTRA_PROFILE = "org.fpsproject.client.extra.PROFILE"
    }

    private var runtime: HeadlessNativeVpnRuntime? = null

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

    internal fun startProfile(profileText: String): VpnRuntimeState {
        val next = HeadlessNativeVpnRuntime.create(profileText, ServicePlatformHooks(this))
        runtime?.stop()
        runtime = next
        return next.start()
    }

    internal fun onLeaseReceived(lease: TunLease): VpnRuntimeState {
        return runtime?.onLeaseReceived(lease) ?: VpnRuntimeState.FAILED
    }

    internal fun stopRuntime(): VpnRuntimeState {
        val stopped = runtime?.stop() ?: VpnRuntimeState.STOPPED
        runtime = null
        return stopped
    }

    internal fun snapshot(): NativeVpnRuntimeSnapshot {
        return runtime?.snapshot() ?: NativeVpnRuntimeSnapshot.stopped()
    }
}

private class ServicePlatformHooks(
    private val service: FpsVpnService,
) : AndroidPlatformHooks {
    private val connectivityManager: ConnectivityManager?
        get() = service.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager

    override fun hasVpnPermission(): Boolean = VpnService.prepare(service) == null

    override fun establishTun(profile: AndroidClientProfile, lease: TunLease): EstablishedTun? {
        return VpnTunEstablisher { AndroidVpnTunnelBuilder(service) }.establish(profile, lease)
    }

    override fun protectSocket(fd: Int): Boolean = service.protect(fd)

    override fun protectSocket(socket: Socket): Boolean = service.protect(socket)

    override fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint> {
        val addresses = underlyingNetwork()?.getAllByName(host) ?: InetAddress.getAllByName(host)
        return addresses.map { ResolvedEndpoint(it.hostAddress ?: it.hostName, port) }
    }

    override fun uidForFlow(flow: TunFlowTuple): Int {
        val manager = connectivityManager ?: return -1
        val protocol = when (flow.protocol) {
            TunProtocol.TCP -> OsConstants.IPPROTO_TCP
            TunProtocol.UDP -> OsConstants.IPPROTO_UDP
        }
        return try {
            manager.getConnectionOwnerUid(
                protocol,
                InetSocketAddress(ipv4Address(flow.sourceIpv4), flow.sourcePort),
                InetSocketAddress(ipv4Address(flow.destinationIpv4), flow.destinationPort),
            )
        } catch (_: RuntimeException) {
            -1
        }
    }

    @Suppress("DEPRECATION")
    private fun underlyingNetwork(): Network? {
        val manager = connectivityManager ?: return null
        return manager.allNetworks.firstOrNull { network ->
            val caps = manager.getNetworkCapabilities(network) ?: return@firstOrNull false
            caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
                !caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)
        } ?: manager.activeNetwork
    }

    private fun ipv4Address(value: Long): InetAddress {
        val normalized = value and 0xffff_ffffL
        return InetAddress.getByAddress(
            byteArrayOf(
                ((normalized ushr 24) and 0xff).toByte(),
                ((normalized ushr 16) and 0xff).toByte(),
                ((normalized ushr 8) and 0xff).toByte(),
                (normalized and 0xff).toByte(),
            ),
        )
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
