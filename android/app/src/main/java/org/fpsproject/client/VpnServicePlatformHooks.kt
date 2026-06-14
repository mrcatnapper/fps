package org.fpsproject.client

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.VpnService
import android.system.OsConstants
import org.fpsproject.client.config.AndroidClientProfile
import org.fpsproject.client.policy.TunFlowTuple
import org.fpsproject.client.policy.TunProtocol
import org.fpsproject.client.runtime.AndroidPlatformHooks
import org.fpsproject.client.runtime.EstablishedTun
import org.fpsproject.client.runtime.ResolvedEndpoint
import org.fpsproject.client.runtime.TunLease
import org.fpsproject.client.runtime.VpnTunEstablisher
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket

internal class VpnServicePlatformHooks(
    private val service: VpnService,
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
        val addresses = if (isIpLiteral(host)) {
            InetAddress.getAllByName(host)
        } else {
            underlyingNetwork()?.getAllByName(host) ?: InetAddress.getAllByName(host)
        }
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

    private fun isIpLiteral(host: String): Boolean {
        return host.contains(':') || host.matches(Regex("""\d{1,3}(\.\d{1,3}){3}"""))
    }
}
