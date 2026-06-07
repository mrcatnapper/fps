package org.fpsproject.client.config

enum class CarrierProbeMode {
    HTTPS_GET,
    WSS,
}

fun parseCarrierProbeMode(value: String, path: String): CarrierProbeMode {
    return when (value) {
        "https_get" -> CarrierProbeMode.HTTPS_GET
        "wss" -> CarrierProbeMode.WSS
        else -> throw AndroidClientProfileParseException("$path must be https_get or wss")
    }
}

data class CarrierProbeProfile(
    val mode: CarrierProbeMode,
    val endpoint: Endpoint,
    val path: String = "/",
    val intervalMs: Long = 10_000,
) {
    init {
        require(path.startsWith('/')) { "path must start with '/'" }
        require(intervalMs > 0) { "intervalMs must be positive" }
    }
}
