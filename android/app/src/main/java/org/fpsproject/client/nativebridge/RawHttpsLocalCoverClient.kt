package org.fpsproject.client.nativebridge

import org.fpsproject.client.config.CarrierProbeMode
import org.fpsproject.client.config.Endpoint
import org.fpsproject.client.runtime.CarrierProbeRuntimePlan
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.Socket
import java.net.SocketTimeoutException
import javax.net.SocketFactory
import javax.net.ssl.SSLException
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory
import kotlin.concurrent.thread

class RawHttpsLocalCoverClientStarter(
    private val socketFactory: SocketFactory = SocketFactory.getDefault(),
    private val sslSocketFactory: SSLSocketFactory = SSLSocketFactory.getDefault() as SSLSocketFactory,
    private val loopbackHost: String = "127.0.0.1",
    private val socketTimeoutMs: Int = 5000,
) : LocalCoverClientStarter {
    override fun start(localBridgePort: Int, carrierPlan: CarrierProbeRuntimePlan): LocalCoverClientStartResult {
        if (carrierPlan.probe.mode != CarrierProbeMode.HTTPS_GET) {
            return LocalCoverClientStartResult.failed("cover_mode_unsupported")
        }
        return try {
            val client = RawHttpsLocalCoverClient(
                localBridgePort = localBridgePort,
                carrierPlan = carrierPlan,
                socketFactory = socketFactory,
                sslSocketFactory = sslSocketFactory,
                loopbackHost = loopbackHost,
                socketTimeoutMs = socketTimeoutMs,
            )
            try {
                client.start()
            } catch (error: Throwable) {
                client.close()
                throw error
            }
            LocalCoverClientStartResult.started(client)
        } catch (_: SSLException) {
            LocalCoverClientStartResult.failed("cover_tls_failed")
        } catch (_: HttpCoverException) {
            LocalCoverClientStartResult.failed("cover_http_failed")
        } catch (_: SocketTimeoutException) {
            LocalCoverClientStartResult.failed("cover_timeout")
        } catch (_: IOException) {
            LocalCoverClientStartResult.failed("cover_io_failed")
        } catch (_: RuntimeException) {
            LocalCoverClientStartResult.failed("cover_io_failed")
        }
    }
}

private class RawHttpsLocalCoverClient(
    private val localBridgePort: Int,
    private val carrierPlan: CarrierProbeRuntimePlan,
    private val socketFactory: SocketFactory,
    private val sslSocketFactory: SSLSocketFactory,
    private val loopbackHost: String,
    private val socketTimeoutMs: Int,
) : LocalCoverClientHandle {
    @Volatile
    private var running = false

    @Volatile
    private var socket: SSLSocket? = null

    private var worker: Thread? = null

    fun start() {
        running = true
        val connected = connect()
        socket = connected
        performGet(connected, carrierPlan.probe.endpoint, carrierPlan.probe.path, carrierPlan.probe.maxResponseBytes)
        worker = thread(start = true, name = "fps-android-raw-https-cover-${carrierPlan.id}", isDaemon = true) {
            runLoop()
        }
    }

    override fun close() {
        running = false
        socket?.closeQuietly()
        val activeWorker = worker
        activeWorker?.interrupt()
        if (activeWorker != null && activeWorker != Thread.currentThread()) {
            try {
                activeWorker.join(250)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
            }
        }
        worker = null
        socket = null
    }

    private fun runLoop() {
        while (running) {
            try {
                Thread.sleep(carrierPlan.probe.intervalMs)
                val active = socket ?: return
                if (running) {
                    performGet(active, carrierPlan.probe.endpoint, carrierPlan.probe.path, carrierPlan.probe.maxResponseBytes)
                }
            } catch (_: InterruptedException) {
                return
            } catch (_: SocketTimeoutException) {
                close()
                return
            } catch (_: IOException) {
                close()
                return
            } catch (_: RuntimeException) {
                close()
                return
            }
        }
    }

    private fun connect(): SSLSocket {
        val plain = socketFactory.createSocket(loopbackHost, localBridgePort)
        plain.soTimeout = socketTimeoutMs
        val endpoint = carrierPlan.probe.endpoint
        val tls = sslSocketFactory.createSocket(plain, endpoint.host, endpoint.port, true) as SSLSocket
        tls.soTimeout = socketTimeoutMs
        tls.sslParameters = tls.sslParameters.also { it.endpointIdentificationAlgorithm = "HTTPS" }
        tls.startHandshake()
        return tls
    }
}

private class HttpCoverException : IOException()

private fun performGet(socket: SSLSocket, endpoint: Endpoint, path: String, maxResponseBytes: Int) {
    val output = socket.outputStream
    val request = "GET $path HTTP/1.1\r\n" +
        "Host: ${hostHeader(endpoint)}\r\n" +
        "User-Agent: fps-android-cover/0\r\n" +
        "Accept: */*\r\n" +
        "Connection: keep-alive\r\n" +
        "\r\n"
    output.write(request.toByteArray(Charsets.US_ASCII))
    output.flush()

    val input = socket.inputStream
    val headers = readHeaders(input = { input.read() })
    val status = parseStatus(headers) ?: throw HttpCoverException()
    if (status !in 200..399) {
        throw HttpCoverException()
    }
    drainBody(status, headers, maxResponseBytes, input = { input.read() })
}

private fun hostHeader(endpoint: Endpoint): String {
    return if (endpoint.port == 443) {
        endpoint.host
    } else {
        "${endpoint.host}:${endpoint.port}"
    }
}

private fun readHeaders(input: () -> Int): String {
    val out = ByteArrayOutputStream()
    var matched = 0
    val marker = byteArrayOf('\r'.code.toByte(), '\n'.code.toByte(), '\r'.code.toByte(), '\n'.code.toByte())
    while (out.size() < MAX_HTTP_HEADERS) {
        val byte = input()
        if (byte < 0) {
            throw IOException("cover_http_eof")
        }
        out.write(byte)
        matched = if (byte.toByte() == marker[matched]) matched + 1 else if (byte.toByte() == marker[0]) 1 else 0
        if (matched == marker.size) {
            return out.toString(Charsets.US_ASCII.name())
        }
    }
    throw IOException("cover_http_headers_too_large")
}

private fun parseStatus(headers: String): Int? {
    val line = headers.lineSequence().firstOrNull() ?: return null
    val parts = line.split(' ', limit = 3)
    return parts.getOrNull(1)?.toIntOrNull()
}

private fun drainBody(status: Int, headers: String, maxResponseBytes: Int, input: () -> Int) {
    if (status == 204 || status == 304) {
        return
    }
    val lengthHeader = headerValue(headers, "content-length")
    val length = lengthHeader?.toIntOrNull()
    if (lengthHeader != null && length == null) {
        throw HttpCoverException()
    }
    if (length != null) {
        if (length < 0 || length > maxResponseBytes) {
            throw HttpCoverException()
        }
        drainExact(length, input, "cover_http_body_eof")
        return
    }
    if (headerValue(headers, "transfer-encoding")?.lowercase()?.contains("chunked") == true) {
        drainChunked(maxResponseBytes, input)
    }
}

private fun headerValue(headers: String, name: String): String? {
    val prefix = "$name:"
    return headers.lineSequence()
        .firstOrNull { it.lowercase().startsWith(prefix) }
        ?.substringAfter(':')
        ?.trim()
}

private fun drainChunked(maxResponseBytes: Int, input: () -> Int) {
    var drained = 0
    while (true) {
        val sizeLine = readLine(input)
        val size = sizeLine.substringBefore(';').trim().toIntOrNull(16) ?: throw HttpCoverException()
        if (size == 0) {
            drainTrailerLines(input)
            return
        }
        if (size < 0 || size > maxResponseBytes - drained) {
            throw HttpCoverException()
        }
        drainExact(size, input, "cover_http_chunk_eof")
        drained += size
        readCrlf(input, "cover_http_chunk_eof")
    }
}

private fun drainTrailerLines(input: () -> Int) {
    while (readLine(input).isNotEmpty()) {
        // Discard trailer metadata.
    }
}

private fun readLine(input: () -> Int): String {
    val out = ByteArrayOutputStream()
    var previous = -1
    while (out.size() < MAX_HTTP_LINE) {
        val byte = input()
        if (byte < 0) {
            throw IOException("cover_http_line_eof")
        }
        if (previous == '\r'.code && byte == '\n'.code) {
            val bytes = out.toByteArray()
            return bytes.copyOf(bytes.size - 1).toString(Charsets.US_ASCII)
        }
        out.write(byte)
        previous = byte
    }
    throw IOException("cover_http_line_too_large")
}

private fun drainExact(size: Int, input: () -> Int, eofError: String) {
    repeat(size) {
        if (input() < 0) {
            throw IOException(eofError)
        }
    }
}

private fun readCrlf(input: () -> Int, eofError: String) {
    val first = input()
    val second = input()
    if (first < 0 || second < 0) {
        throw IOException(eofError)
    }
    if (first != '\r'.code || second != '\n'.code) {
        throw HttpCoverException()
    }
}

private fun Socket.closeQuietly() {
    try {
        close()
    } catch (_: IOException) {
        // Best-effort cleanup only.
    } catch (_: RuntimeException) {
        // Best-effort cleanup only.
    }
}

private const val MAX_HTTP_HEADERS = 64 * 1024
private const val MAX_HTTP_LINE = 8 * 1024
