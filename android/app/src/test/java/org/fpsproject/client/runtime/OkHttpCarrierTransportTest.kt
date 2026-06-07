package org.fpsproject.client.runtime

import mockwebserver3.MockResponse
import mockwebserver3.MockWebServer
import okhttp3.OkHttpClient
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okhttp3.Response
import okhttp3.tls.HandshakeCertificates
import okhttp3.tls.HeldCertificate
import org.fpsproject.client.config.CarrierProbeMode
import org.fpsproject.client.config.CarrierProbeProfile
import org.fpsproject.client.config.Endpoint
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class OkHttpCarrierTransportTest {
    private val servers = mutableListOf<MockWebServer>()

    @After
    fun tearDown() {
        servers.forEach { it.close() }
    }

    @Test
    fun httpsGetUsesProtectedSocketAndUnderlyingEndpoint() {
        val server = newHttpsServer("carrier.example.test")
        server.enqueue(MockResponse.Builder().code(204).build())
        val hooks = FakeOkHttpHooks(server)
        val factory = OkHttpCarrierTransportFactory(hooks, tlsClientBuilder(server))
        val transport = factory.create(plan(CarrierProbeMode.HTTPS_GET, server.port, "/ping"))

        val connected = transport.connect(ResolvedEndpoint("127.0.0.1", server.port))
        val request = server.takeRequest(2, TimeUnit.SECONDS)

        assertTrue(connected.toString(), connected.ok)
        assertNotNull(request)
        assertEquals("/ping", request!!.url.encodedPath)
        assertTrue(hooks.protectedJavaSockets > 0)
        assertTrue(transport.protectsSocketsInternally)
    }

    @Test
    fun httpsGetFailureReturnsNonSecretErrorName() {
        val server = newHttpsServer("carrier.example.test")
        server.enqueue(MockResponse.Builder().code(503).body("nope").build())
        val hooks = FakeOkHttpHooks(server)
        val factory = OkHttpCarrierTransportFactory(hooks, tlsClientBuilder(server))
        val transport = factory.create(plan(CarrierProbeMode.HTTPS_GET, server.port, "/ping"))

        val connected = transport.connect(ResolvedEndpoint("127.0.0.1", server.port))

        assertFalse(connected.ok)
        assertEquals("http_status_503", connected.error)
    }

    @Test
    fun socketProtectionFailurePreventsHttpsConnect() {
        val server = newHttpsServer("carrier.example.test")
        server.enqueue(MockResponse.Builder().code(204).build())
        val hooks = FakeOkHttpHooks(server, protectJavaSockets = false)
        val factory = OkHttpCarrierTransportFactory(hooks, tlsClientBuilder(server))
        val transport = factory.create(plan(CarrierProbeMode.HTTPS_GET, server.port, "/ping"))

        val connected = transport.connect(ResolvedEndpoint("127.0.0.1", server.port))

        assertFalse(connected.ok)
        assertEquals("socket_protect_failed", connected.error)
        assertEquals(0, server.requestCount)
    }

    @Test
    fun wssOpensAndSendsProbeMessages() {
        val server = newHttpsServer("carrier.example.test")
        val messages = mutableListOf<String>()
        val firstMessage = CountDownLatch(1)
        server.enqueue(
            MockResponse.Builder()
                .webSocketUpgrade(object : WebSocketListener() {
                    override fun onMessage(webSocket: WebSocket, text: String) {
                        messages += text
                        firstMessage.countDown()
                        webSocket.send(text)
                    }
                })
                .build(),
        )
        val hooks = FakeOkHttpHooks(server)
        val factory = OkHttpCarrierTransportFactory(hooks, tlsClientBuilder(server))
        val transport = factory.create(plan(CarrierProbeMode.WSS, server.port, "/stream"))

        val connected = transport.connect(ResolvedEndpoint("127.0.0.1", server.port))
        val probe = transport.probe(1234)

        assertTrue(connected.toString(), connected.ok)
        assertTrue(probe.toString(), probe.ok)
        assertTrue(firstMessage.await(2, TimeUnit.SECONDS))
        assertEquals(listOf("fps-probe:1:1234:1"), messages)
        assertTrue(hooks.protectedJavaSockets > 0)
    }

    @Test
    fun wssFailureReturnsNonSecretErrorName() {
        val server = newHttpsServer("carrier.example.test")
        server.enqueue(
            MockResponse.Builder()
                .webSocketUpgrade(object : WebSocketListener() {
                    override fun onMessage(webSocket: WebSocket, text: String) = Unit
                })
                .build(),
        )
        val hooks = FakeOkHttpHooks(server)
        val factory = OkHttpCarrierTransportFactory(hooks, tlsClientBuilder(server))
        val transport = factory.create(plan(CarrierProbeMode.WSS, server.port, "/stream"))

        assertTrue(transport.connect(ResolvedEndpoint("127.0.0.1", server.port)).ok)
        server.close()

        var result = CarrierTransportResult.success()
        var attempt = 0
        while (attempt < 20 && result.ok) {
            result = transport.probe(1L + attempt)
            Thread.sleep(50)
            attempt += 1
        }

        assertFalse(result.ok)
        assertNotNull(result.error)
    }

    private fun newHttpsServer(hostname: String): HttpsFixture {
        val heldCertificate = HeldCertificate.Builder()
            .commonName(hostname)
            .addSubjectAlternativeName(hostname)
            .build()
        val serverCertificates = HandshakeCertificates.Builder()
            .heldCertificate(heldCertificate)
            .build()
        val clientCertificates = HandshakeCertificates.Builder()
            .addTrustedCertificate(heldCertificate.certificate)
            .build()
        val server = MockWebServer()
        server.useHttps(serverCertificates.sslSocketFactory())
        server.start()
        servers += server
        return HttpsFixture(server, clientCertificates)
    }

    private fun tlsClientBuilder(fixture: HttpsFixture): OkHttpClient.Builder {
        return OkHttpClient.Builder()
            .sslSocketFactory(fixture.clientCertificates.sslSocketFactory(), fixture.clientCertificates.trustManager)
    }

    private fun plan(mode: CarrierProbeMode, port: Int, path: String): CarrierRuntimePlan {
        return CarrierRuntimePlan(
            id = 1,
            probe = CarrierProbeProfile(
                mode = mode,
                endpoint = Endpoint("carrier.example.test", port),
                path = path,
                intervalMs = 1000,
            ),
        )
    }

}

private data class HttpsFixture(
    val server: MockWebServer,
    val clientCertificates: HandshakeCertificates,
) {
    val port: Int get() = server.port

    fun enqueue(response: MockResponse) = server.enqueue(response)

    fun takeRequest(timeout: Long, unit: TimeUnit) = server.takeRequest(timeout, unit)

    fun close() = server.close()

    val requestCount: Int get() = server.requestCount
}

private class FakeOkHttpHooks(
    private val fixture: HttpsFixture,
    private val protectJavaSockets: Boolean = true,
) : AndroidPlatformHooks {
    var protectedJavaSockets = 0

    override fun hasVpnPermission() = true

    override fun establishTun(
        profile: org.fpsproject.client.config.AndroidClientProfile,
        lease: TunLease,
    ) = EstablishedTun.borrowed(fd = 7, mtu = lease.mtu)

    override fun protectSocket(fd: Int) = true

    override fun protectSocket(socket: Socket): Boolean {
        protectedJavaSockets += 1
        return protectJavaSockets
    }

    override fun resolveOnUnderlyingNetwork(host: String, port: Int): List<ResolvedEndpoint> {
        return listOf(ResolvedEndpoint("127.0.0.1", fixture.port))
    }

    override fun uidForFlow(flow: org.fpsproject.client.policy.TunFlowTuple) = -1
}
