package org.fpsproject.client.runtime

import okhttp3.Dns
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.fpsproject.client.config.CarrierProbeMode
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.time.Duration
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import javax.net.SocketFactory

class OkHttpCarrierProbeTransportFactory(
    private val hooks: AndroidPlatformHooks,
    clientBuilder: OkHttpClient.Builder = OkHttpClient.Builder(),
    private val connectTimeoutMs: Long = 5000,
) : CarrierProbeTransportFactory {
    private val templateClient = clientBuilder.build()

    override fun create(plan: CarrierProbeRuntimePlan): CarrierProbeTransport {
        return when (plan.probe.mode) {
            CarrierProbeMode.HTTPS_GET -> OkHttpHttpsGetCarrierProbeTransport(plan, hooks, templateClient, connectTimeoutMs)
            CarrierProbeMode.WSS -> OkHttpWssCarrierProbeTransport(plan, hooks, templateClient, connectTimeoutMs)
        }
    }
}

private abstract class BaseOkHttpCarrierProbeTransport(
    protected val plan: CarrierProbeRuntimePlan,
    private val hooks: AndroidPlatformHooks,
    private val templateClient: OkHttpClient,
    protected val connectTimeoutMs: Long,
) : CarrierProbeTransport {
    override val socketFd: Int = -1
    override val protectsSocketsInternally: Boolean = true

    protected var client: OkHttpClient? = null

    protected fun buildClient(endpoint: ResolvedEndpoint): OkHttpClient {
        val built = templateClient.newBuilder()
            .socketFactory(ProtectingSocketFactory(SocketFactory.getDefault(), hooks))
            .dns(SingleEndpointDns(plan.probe.endpoint.host, endpoint))
            .connectTimeout(Duration.ofMillis(connectTimeoutMs))
            .readTimeout(Duration.ofMillis(connectTimeoutMs))
            .callTimeout(Duration.ofMillis(connectTimeoutMs))
            .build()
        client = built
        return built
    }

    protected fun request(scheme: String): Request {
        val url = okhttp3.HttpUrl.Builder()
            .scheme(scheme)
            .host(plan.probe.endpoint.host)
            .port(plan.probe.endpoint.port)
            .encodedPath(plan.probe.path)
            .build()
        return Request.Builder().url(url).build()
    }

    protected fun errorName(error: Throwable): String {
        return when (error.message) {
            "socket_protect_failed" -> "socket_protect_failed"
            else -> error::class.java.simpleName.ifEmpty { "io_error" }
        }
    }

    override fun close() {
        client?.dispatcher?.cancelAll()
        client?.connectionPool?.evictAll()
        client = null
    }
}

private class OkHttpHttpsGetCarrierProbeTransport(
    plan: CarrierProbeRuntimePlan,
    hooks: AndroidPlatformHooks,
    templateClient: OkHttpClient,
    connectTimeoutMs: Long,
) : BaseOkHttpCarrierProbeTransport(plan, hooks, templateClient, connectTimeoutMs) {
    private var activeClient: OkHttpClient? = null

    override fun connect(endpoint: ResolvedEndpoint): CarrierProbeResult {
        activeClient = buildClient(endpoint)
        return getOnce()
    }

    override fun probe(nowMs: Long): CarrierProbeResult = getOnce()

    private fun getOnce(): CarrierProbeResult {
        val current = activeClient ?: return CarrierProbeResult.failure("transport_not_connected")
        return try {
            current.newCall(request("https")).execute().use { response ->
                response.toResult()
            }
        } catch (error: IOException) {
            CarrierProbeResult.failure(errorName(error))
        }
    }

    private fun Response.toResult(): CarrierProbeResult {
        return if (code in 200..399) {
            CarrierProbeResult.success()
        } else {
            CarrierProbeResult.failure("http_status_$code")
        }
    }
}

private class OkHttpWssCarrierProbeTransport(
    plan: CarrierProbeRuntimePlan,
    hooks: AndroidPlatformHooks,
    templateClient: OkHttpClient,
    connectTimeoutMs: Long,
) : BaseOkHttpCarrierProbeTransport(plan, hooks, templateClient, connectTimeoutMs) {
    @Volatile
    private var webSocket: WebSocket? = null

    @Volatile
    private var closedError: String? = null

    private val probeSequence = AtomicInteger(0)

    override fun connect(endpoint: ResolvedEndpoint): CarrierProbeResult {
        val current = buildClient(endpoint)
        val opened = CountDownLatch(1)
        val failed = CountDownLatch(1)
        val listener = object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                this@OkHttpWssCarrierProbeTransport.webSocket = webSocket
                opened.countDown()
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                // Echoes prove the peer is alive, but probe send success is the
                // synchronous carrier-manager contract for this headless layer.
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                closedError = "websocket_closed"
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                closedError = "websocket_closed"
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                closedError = errorName(t)
                failed.countDown()
            }
        }

        current.newWebSocket(request("https"), listener)
        val openedOk = opened.await(connectTimeoutMs, TimeUnit.MILLISECONDS)
        if (openedOk) {
            return CarrierProbeResult.success()
        }
        if (failed.count == 0L) {
            return CarrierProbeResult.failure(closedError ?: "websocket_connect_failed")
        }
        return CarrierProbeResult.failure("websocket_connect_timeout")
    }

    override fun probe(nowMs: Long): CarrierProbeResult {
        closedError?.let { return CarrierProbeResult.failure(it) }
        val active = webSocket ?: return CarrierProbeResult.failure("transport_not_connected")
        val message = "fps-probe:${plan.id}:$nowMs:${probeSequence.incrementAndGet()}"
        return if (active.send(message)) {
            closedError?.let { CarrierProbeResult.failure(it) } ?: CarrierProbeResult.success()
        } else {
            CarrierProbeResult.failure("websocket_send_failed")
        }
    }

    override fun close() {
        webSocket?.close(1000, "fps carrier stop")
        webSocket = null
        super.close()
    }
}

private class SingleEndpointDns(
    private val expectedHost: String,
    private val endpoint: ResolvedEndpoint,
) : Dns {
    override fun lookup(hostname: String): List<InetAddress> {
        if (hostname == expectedHost) {
            return listOf(InetAddress.getByName(endpoint.address))
        }
        return Dns.SYSTEM.lookup(hostname)
    }
}

private class ProtectingSocketFactory(
    private val delegate: SocketFactory,
    private val hooks: AndroidPlatformHooks,
) : SocketFactory() {
    override fun createSocket(): Socket = protect(delegate.createSocket())

    override fun createSocket(host: String, port: Int): Socket {
        return connect(createSocket(), InetSocketAddress(host, port))
    }

    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket {
        val socket = createSocket()
        socket.bind(InetSocketAddress(localHost, localPort))
        return connect(socket, InetSocketAddress(host, port))
    }

    override fun createSocket(host: InetAddress, port: Int): Socket {
        return connect(createSocket(), InetSocketAddress(host, port))
    }

    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket {
        val socket = createSocket()
        socket.bind(InetSocketAddress(localAddress, localPort))
        return connect(socket, InetSocketAddress(address, port))
    }

    private fun connect(socket: Socket, endpoint: InetSocketAddress): Socket {
        socket.connect(endpoint)
        return socket
    }

    private fun protect(socket: Socket): Socket {
        if (!hooks.protectSocket(socket)) {
            socket.close()
            throw IOException("socket_protect_failed")
        }
        return socket
    }
}
