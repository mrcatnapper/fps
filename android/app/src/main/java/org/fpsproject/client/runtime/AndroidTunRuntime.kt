package org.fpsproject.client.runtime

import org.fpsproject.client.config.AndroidClientProfile
import kotlin.math.min

data class Ipv4Cidr(
    val address: String,
    val prefixLength: Int,
) {
    init {
        require(prefixLength in 0..32) { "prefixLength must be in 0..32" }
    }
}

data class AndroidTunPlan(
    val sessionName: String,
    val mtu: Int,
    val addresses: List<Ipv4Cidr>,
    val routes: List<Ipv4Cidr>,
    val dnsServers: List<String> = emptyList(),
)

interface TunHandle : AutoCloseable {
    val fd: Int

    override fun close()
}

interface VpnTunnelBuilder {
    fun setSession(name: String)

    fun setMtu(mtu: Int)

    fun addAddress(address: String, prefixLength: Int)

    fun addRoute(address: String, prefixLength: Int)

    fun addDnsServer(address: String)

    fun establish(): TunHandle?
}

class VpnTunEstablisher(
    private val builderFactory: () -> VpnTunnelBuilder,
) {
    fun establish(profile: AndroidClientProfile, lease: TunLease): EstablishedTun? {
        val plan = buildAndroidTunPlan(profile, lease)
        val builder = builderFactory()
        builder.setSession(plan.sessionName)
        builder.setMtu(plan.mtu)
        plan.addresses.forEach { builder.addAddress(it.address, it.prefixLength) }
        plan.routes.forEach { builder.addRoute(it.address, it.prefixLength) }
        plan.dnsServers.forEach { builder.addDnsServer(it) }
        val handle = builder.establish() ?: return null
        return EstablishedTun.owned(fd = handle.fd, mtu = plan.mtu, handle = handle)
    }
}

fun buildAndroidTunPlan(profile: AndroidClientProfile, lease: TunLease): AndroidTunPlan {
    val profileMtu = profile.tun?.mtu ?: lease.mtu
    val mtu = min(profileMtu, lease.mtu)
    val sessionName = profile.tun?.name?.takeIf { it.isNotBlank() } ?: "FPS"
    return AndroidTunPlan(
        sessionName = sessionName,
        mtu = mtu,
        addresses = listOf(Ipv4Cidr(ipv4ToString(lease.clientIpv4), lease.prefixLength)),
        routes = listOf(Ipv4Cidr(ipv4ToString(networkIpv4(lease.clientIpv4, lease.prefixLength)), lease.prefixLength)),
    )
}

fun ipv4ToString(value: Long): String {
    val normalized = value and 0xffff_ffffL
    return listOf(
        (normalized ushr 24) and 0xff,
        (normalized ushr 16) and 0xff,
        (normalized ushr 8) and 0xff,
        normalized and 0xff,
    ).joinToString(".")
}

private fun networkIpv4(value: Long, prefixLength: Int): Long {
    require(prefixLength in 0..32) { "prefixLength must be in 0..32" }
    val normalized = value and 0xffff_ffffL
    val mask = if (prefixLength == 0) {
        0L
    } else {
        (0xffff_ffffL shl (32 - prefixLength)) and 0xffff_ffffL
    }
    return normalized and mask
}
