package org.fpsproject.client.policy

enum class TunProtocol {
    TCP,
    UDP,
}

data class TunFlowTuple(
    val protocol: TunProtocol,
    val sourceIpv4: Long,
    val sourcePort: Int,
    val destinationIpv4: Long,
    val destinationPort: Int,
)
