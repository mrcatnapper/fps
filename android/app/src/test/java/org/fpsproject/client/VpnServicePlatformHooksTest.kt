package org.fpsproject.client

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VpnServicePlatformHooksTest {
    @Test
    fun ipLiteralDetectionAcceptsIpv4AndIpv6Literals() {
        assertTrue(isIpLiteralHost("127.0.0.1"))
        assertTrue(isIpLiteralHost("10.66.0.1"))
        assertTrue(isIpLiteralHost("2001:db8::1"))
        assertTrue(isIpLiteralHost("::1"))
    }

    @Test
    fun ipLiteralDetectionRejectsHostnamesAndMalformedIpv4() {
        assertFalse(isIpLiteralHost("fps.example.test"))
        assertFalse(isIpLiteralHost("localhost"))
        assertFalse(isIpLiteralHost("999.0.0.1"))
        assertFalse(isIpLiteralHost("127.0.0"))
        assertFalse(isIpLiteralHost("127.0.0.1.example"))
    }
}
