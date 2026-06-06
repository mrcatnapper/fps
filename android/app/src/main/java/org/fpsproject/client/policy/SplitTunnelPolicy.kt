package org.fpsproject.client.policy

fun interface UidResolver {
    fun ownerUidFor(flow: TunFlowTuple): Int
}

enum class SplitTunnelDecision {
    ALLOW,
    DROP,
}

class SplitTunnelPolicy(
    private val allowedUids: Set<Int>,
    private val uidResolver: UidResolver,
) {
    fun decide(flow: TunFlowTuple?): SplitTunnelDecision {
        if (flow == null) {
            return SplitTunnelDecision.DROP
        }
        val ownerUid = uidResolver.ownerUidFor(flow)
        if (ownerUid < 0) {
            return SplitTunnelDecision.DROP
        }
        return if (ownerUid in allowedUids) {
            SplitTunnelDecision.ALLOW
        } else {
            SplitTunnelDecision.DROP
        }
    }
}
