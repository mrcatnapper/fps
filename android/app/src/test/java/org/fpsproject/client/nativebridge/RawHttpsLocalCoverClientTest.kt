package org.fpsproject.client.nativebridge

import mockwebserver3.MockResponse
import mockwebserver3.MockWebServer
import okhttp3.tls.HandshakeCertificates
import okhttp3.tls.HeldCertificate
import org.fpsproject.client.config.CarrierProbeMode
import org.fpsproject.client.config.CarrierProbeProfile
import org.fpsproject.client.config.Endpoint
import org.fpsproject.client.runtime.CarrierProbeRuntimePlan
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class RawHttpsLocalCoverClientTest {
    private val servers = mutableListOf<MockWebServer>()
    private val bridges = mutableListOf<LocalTcpBridge>()

    @After
    fun tearDown() {
        bridges.forEach { it.close() }
        servers.forEach { it.close() }
    }

    @Test
    fun rawHttpsCoverClientSendsKeepAliveGetsThroughLocalBridge() {
        val fixture = newHttpsServer("carrier.example.test")
        fixture.enqueue(MockResponse.Builder().code(204).build())
        fixture.enqueue(MockResponse.Builder().code(200).body("ok").build())
        val bridge = newBridge(fixture.port)
        val starter = RawHttpsLocalCoverClientStarter(
            sslSocketFactory = fixture.clientCertificates.sslSocketFactory(),
            socketTimeoutMs = 2000,
        )

        val result = starter.start(bridge.port, httpsPlan(path = "/ping", intervalMs = 50))

        assertNull(result.error)
        assertNotNull(result.handle)
        val first = fixture.takeRequest(2, TimeUnit.SECONDS)
        val second = fixture.takeRequest(2, TimeUnit.SECONDS)
        assertNotNull(first)
        assertNotNull(second)
        assertEquals("/ping", first!!.url.encodedPath)
        assertEquals("/ping", second!!.url.encodedPath)
        assertEquals("carrier.example.test", first.headers["Host"])
        assertEquals("carrier.example.test", second.headers["Host"])

        result.handle!!.close()
    }

    @Test
    fun unsupportedModeFailsWithoutNetworkUse() {
        val starter = RawHttpsLocalCoverClientStarter(socketTimeoutMs = 250)

        val result = starter.start(1, wssPlan())

        assertNull(result.handle)
        assertEquals("cover_mode_unsupported", result.error)
    }

    @Test
    fun httpStatusFailureReturnsMetadataError() {
        val fixture = newHttpsServer("carrier.example.test")
        fixture.enqueue(MockResponse.Builder().code(503).body("nope").build())
        val bridge = newBridge(fixture.port)
        val starter = RawHttpsLocalCoverClientStarter(
            sslSocketFactory = fixture.clientCertificates.sslSocketFactory(),
            socketTimeoutMs = 2000,
        )

        val result = starter.start(bridge.port, httpsPlan(path = "/ping"))

        assertNull(result.handle)
        assertEquals("cover_http_failed", result.error)
    }

    @Test
    fun tlsFailureReturnsMetadataError() {
        val fixture = newHttpsServer("carrier.example.test")
        fixture.enqueue(MockResponse.Builder().code(204).build())
        val bridge = newBridge(fixture.port)
        val starter = RawHttpsLocalCoverClientStarter(socketTimeoutMs = 2000)

        val result = starter.start(bridge.port, httpsPlan(path = "/ping"))

        assertNull(result.handle)
        assertEquals("cover_tls_failed", result.error)
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

    private fun newBridge(targetPort: Int): LocalTcpBridge {
        val bridge = LocalTcpBridge(targetPort)
        bridges += bridge
        return bridge
    }

    private fun httpsPlan(path: String, intervalMs: Long = 1000): CarrierProbeRuntimePlan {
        return CarrierProbeRuntimePlan(
            id = 0,
            probe = CarrierProbeProfile(
                mode = CarrierProbeMode.HTTPS_GET,
                endpoint = Endpoint("carrier.example.test", 443),
                path = path,
                intervalMs = intervalMs,
            ),
        )
    }

    private fun wssPlan(): CarrierProbeRuntimePlan {
        return CarrierProbeRuntimePlan(
            id = 1,
            probe = CarrierProbeProfile(
                mode = CarrierProbeMode.WSS,
                endpoint = Endpoint("carrier.example.test", 443),
                path = "/stream",
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
}

private class LocalTcpBridge(
    private val targetPort: Int,
) : AutoCloseable {
    private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
    private val sockets = CopyOnWriteArrayList<Socket>()

    @Volatile
    private var closed = false

    private val acceptThread = thread(start = true, isDaemon = true, name = "fps-test-local-cover-bridge") {
        acceptLoop()
    }

    val port: Int get() = server.localPort

    override fun close() {
        closed = true
        server.closeQuietly()
        sockets.forEach { it.closeQuietly() }
        acceptThread.interrupt()
    }

    private fun acceptLoop() {
        while (!closed) {
            try {
                val left = server.accept()
                val right = Socket("127.0.0.1", targetPort)
                sockets += left
                sockets += right
                pump(left, right)
                pump(right, left)
            } catch (_: IOException) {
                if (!closed) {
                    return
                }
            }
        }
    }

    private fun pump(from: Socket, to: Socket) {
        thread(start = true, isDaemon = true, name = "fps-test-local-cover-pump") {
            try {
                from.getInputStream().copyTo(to.getOutputStream())
            } catch (_: IOException) {
                // Closing either side is enough to end a test bridge stream.
            } finally {
                from.closeQuietly()
                to.closeQuietly()
            }
        }
    }
}

private fun Socket.closeQuietly() {
    try {
        close()
    } catch (_: IOException) {
        // Best-effort cleanup.
    }
}

private fun ServerSocket.closeQuietly() {
    try {
        close()
    } catch (_: IOException) {
        // Best-effort cleanup.
    }
}
