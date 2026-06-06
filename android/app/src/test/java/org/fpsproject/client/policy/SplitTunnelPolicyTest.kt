package org.fpsproject.client.policy

import org.junit.Assert.assertEquals
import org.junit.Test

class SplitTunnelPolicyTest {
    private val flow = TunFlowTuple(
        protocol = TunProtocol.TCP,
        sourceIpv4 = 0x0a420002,
        sourcePort = 53000,
        destinationIpv4 = 0x5db8d822,
        destinationPort = 443,
    )

    @Test
    fun allowsConfiguredUid() {
        val policy = SplitTunnelPolicy(setOf(10042), UidResolver { 10042 })

        assertEquals(SplitTunnelDecision.ALLOW, policy.decide(flow))
    }

    @Test
    fun dropsUnknownFlow() {
        val policy = SplitTunnelPolicy(setOf(10042), UidResolver { 10042 })

        assertEquals(SplitTunnelDecision.DROP, policy.decide(null))
    }

    @Test
    fun dropsInvalidUid() {
        val policy = SplitTunnelPolicy(setOf(10042), UidResolver { -1 })

        assertEquals(SplitTunnelDecision.DROP, policy.decide(flow))
    }

    @Test
    fun dropsUidOutsideAllowlist() {
        val policy = SplitTunnelPolicy(setOf(10042), UidResolver { 10043 })

        assertEquals(SplitTunnelDecision.DROP, policy.decide(flow))
    }
}
