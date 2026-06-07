package org.fpsproject.client.config

import org.json.JSONException
import org.json.JSONArray
import org.json.JSONObject
import java.nio.charset.StandardCharsets
import java.util.Base64
import java.util.Locale
import java.util.UUID

private const val PROFILE_URI_PREFIX = "fps://v1/"

class AndroidClientProfileParseException(message: String) : IllegalArgumentException(message)

data class Endpoint(val host: String, val port: Int) {
    override fun toString(): String {
        val displayHost = if (host.contains(':') && !host.startsWith("[")) "[$host]" else host
        return "$displayHost:$port"
    }
}

data class CodecProfile(
    val maxFramePayload: Int = 16 * 1024,
    val maxFramePadding: Int = 2048,
    val allowFragmentation: Boolean = true,
)

data class TunProfile(
    val enabled: Boolean = false,
    val name: String? = null,
    val mtu: Int = 1280,
    val autoConfigure: Boolean = false,
)

data class OpsProfile(
    val statusSocket: String? = null,
)

class SplitTunnelProfile(
    val allowedUids: Set<Int> = emptySet(),
) {
    override fun toString(): String {
        return "SplitTunnelProfile(allowedUidCount=${allowedUids.size})"
    }
}

class ZeroRttProfile(
    val profileId: String,
    val clientUuid: String,
    val serverPublicKeyBase64: String,
    val clientUpgradeDelayMs: Long = 2000,
    val clientUpgradeDelaySigmaMs: Long = clientUpgradeDelayMs / 3,
) {
    override fun toString(): String {
        return "ZeroRttProfile(profileId=$profileId, clientUuid=<redacted>, " +
            "serverPublicKeyBase64=<redacted>, clientUpgradeDelayMs=$clientUpgradeDelayMs, " +
            "clientUpgradeDelaySigmaMs=$clientUpgradeDelaySigmaMs)"
    }
}

class AndroidClientProfile(
    val server: Endpoint,
    val zeroRtt: ZeroRttProfile,
    val codec: CodecProfile = CodecProfile(),
    val tun: TunProfile? = null,
    val ops: OpsProfile = OpsProfile(),
    val carriers: List<CarrierProbeProfile> = emptyList(),
    val splitTunnel: SplitTunnelProfile = SplitTunnelProfile(),
) {
    override fun toString(): String {
        return "AndroidClientProfile(server=$server, zeroRtt=$zeroRtt, codec=$codec, tun=$tun, " +
            "ops=$ops, carriers=$carriers, splitTunnel=$splitTunnel)"
    }
}

object AndroidClientProfileParser {
    fun parse(text: String): AndroidClientProfile {
        val jsonText = if (text.trimStart().startsWith(PROFILE_URI_PREFIX)) {
            decodeProfileUri(text.trim())
        } else {
            text
        }
        val rootObject = try {
            JSONObject(jsonText)
        } catch (error: JSONException) {
            throw AndroidClientProfileParseException(error.message ?: "invalid JSON")
        }
        rejectServerOnlyFields(rootObject)

        val server = parseEndpoint(rootObject.requiredString("network.server"), "network.server")
        val zeroRttObject = rootObject.requiredObject("security.zero_rtt")
        if (!zeroRttObject.optionalBoolean("enabled", true, "security.zero_rtt.enabled")) {
            throw AndroidClientProfileParseException("security.zero_rtt.enabled must be true")
        }
        val profileId = zeroRttObject.requiredString("profile_id", "security.zero_rtt.profile_id")
        val clientUuid = canonicalClientUuid(zeroRttObject.requiredString("client_uuid", "security.zero_rtt.client_uuid"))
        val serverPublicKey = zeroRttObject.requiredString("server_public_key_base64", "security.zero_rtt.server_public_key_base64")
        validateServerPublicKey(serverPublicKey)
        val upgradeDelayMs = zeroRttObject.optionalLong("client_upgrade_delay_ms", 2000, "security.zero_rtt.client_upgrade_delay_ms")
        val defaultSigmaMs = upgradeDelayMs / 3
        val upgradeSigmaMs =
            zeroRttObject.optionalLong("client_upgrade_delay_sigma_ms", defaultSigmaMs, "security.zero_rtt.client_upgrade_delay_sigma_ms")

        return AndroidClientProfile(
            server = server,
            zeroRtt = ZeroRttProfile(
                profileId = profileId,
                clientUuid = clientUuid,
                serverPublicKeyBase64 = serverPublicKey,
                clientUpgradeDelayMs = upgradeDelayMs,
                clientUpgradeDelaySigmaMs = upgradeSigmaMs,
            ),
            codec = parseCodec(rootObject.optionalObject("codec")),
            tun = parseTun(rootObject.optionalObject("tun")),
            ops = parseOps(rootObject.optionalObject("ops")),
            carriers = parseCarriers(rootObject.optionalArray("carriers")),
            splitTunnel = parseSplitTunnel(rootObject.optionalObject("split_tunnel")),
        )
    }

    private fun decodeProfileUri(uri: String): String {
        val payload = uri.removePrefix(PROFILE_URI_PREFIX)
        if (payload.isEmpty()) {
            throw AndroidClientProfileParseException("empty fps://v1 profile payload")
        }
        val padded = payload + "=".repeat((4 - payload.length % 4) % 4)
        val decoded = try {
            Base64.getUrlDecoder().decode(padded)
        } catch (_: IllegalArgumentException) {
            throw AndroidClientProfileParseException("invalid fps://v1 base64url payload")
        }
        return String(decoded, StandardCharsets.UTF_8)
    }

    private fun rejectServerOnlyFields(root: JSONObject) {
        val forbidden = listOf(
            "security.zero_rtt.server_private_key_base64",
            "security.zero_rtt.allowed_client_uuids",
            "security.zero_rtt.allowed_client_public_keys",
            "tun.lease_pool",
            "tun.server_address",
            "tun.lease_file",
            "tun.client_isolation",
        )
        val present = forbidden.firstOrNull { root.find(it) != null }
        if (present != null) {
            throw AndroidClientProfileParseException("$present is not valid in an Android client profile")
        }
    }

    private fun parseCodec(codec: JSONObject?): CodecProfile {
        if (codec == null) {
            return CodecProfile()
        }
        return CodecProfile(
            maxFramePayload = codec.optionalInt("max_frame_payload", 16 * 1024, "codec.max_frame_payload"),
            maxFramePadding = codec.optionalInt("max_frame_padding", 2048, "codec.max_frame_padding"),
            allowFragmentation = codec.optionalBoolean("allow_fragmentation", true, "codec.allow_fragmentation"),
        )
    }

    private fun parseTun(tun: JSONObject?): TunProfile? {
        if (tun == null) {
            return null
        }
        val enabled = tun.optionalBoolean("enabled", false, "tun.enabled")
        return TunProfile(
            enabled = enabled,
            name = tun.optionalString("name", "tun.name"),
            mtu = tun.optionalInt("mtu", 1280, "tun.mtu"),
            autoConfigure = tun.optionalBoolean("auto_configure", false, "tun.auto_configure"),
        )
    }

    private fun parseOps(ops: JSONObject?): OpsProfile {
        return OpsProfile(statusSocket = ops?.optionalString("status_socket", "ops.status_socket"))
    }

    private fun parseCarriers(carriers: JSONArray?): List<CarrierProbeProfile> {
        if (carriers == null) {
            return emptyList()
        }
        return List(carriers.length()) { index ->
            val path = "carriers[$index]"
            val carrier = carriers.get(index) as? JSONObject
                ?: throw AndroidClientProfileParseException("$path must be a JSON object")
            CarrierProbeProfile(
                mode = parseCarrierProbeMode(carrier.requiredString("mode", "$path.mode"), "$path.mode"),
                endpoint = parseEndpoint(carrier.requiredString("endpoint", "$path.endpoint"), "$path.endpoint"),
                path = parseCarrierPath(carrier.optionalString("path", "$path.path") ?: "/", "$path.path"),
                intervalMs = carrier.optionalPositiveLong("interval_ms", 10_000, "$path.interval_ms"),
            )
        }
    }

    private fun parseCarrierPath(value: String, path: String): String {
        if (!value.startsWith('/')) {
            throw AndroidClientProfileParseException("$path must start with '/'")
        }
        return value
    }

    private fun parseSplitTunnel(splitTunnel: JSONObject?): SplitTunnelProfile {
        if (splitTunnel == null) {
            return SplitTunnelProfile()
        }
        val allowedUids = splitTunnel.optionalArray("allowed_uids") ?: return SplitTunnelProfile()
        return SplitTunnelProfile(
            allowedUids = List(allowedUids.length()) { index ->
                val value = allowedUids.get(index)
                val path = "split_tunnel.allowed_uids[$index]"
                when (value) {
                    is Int -> value
                    is Long -> value.toIntIfExact(path)
                    else -> throw AndroidClientProfileParseException("$path must be an integer")
                }.also {
                    if (it < 0) {
                        throw AndroidClientProfileParseException("$path must be non-negative")
                    }
                }
            }.toSet(),
        )
    }

    private fun canonicalClientUuid(value: String): String {
        val parsed = try {
            UUID.fromString(value)
        } catch (_: IllegalArgumentException) {
            throw AndroidClientProfileParseException("security.zero_rtt.client_uuid must be a canonical UUIDv4")
        }
        val canonical = parsed.toString()
        if (value.lowercase(Locale.US) != canonical || parsed.version() != 4) {
            throw AndroidClientProfileParseException("security.zero_rtt.client_uuid must be a canonical UUIDv4")
        }
        return canonical
    }

    private fun validateServerPublicKey(value: String) {
        if (value.length % 4 != 0) {
            throw AndroidClientProfileParseException("security.zero_rtt.server_public_key_base64 must be padded RFC4648 base64")
        }
        val decoded = try {
            Base64.getDecoder().decode(value)
        } catch (_: IllegalArgumentException) {
            throw AndroidClientProfileParseException("security.zero_rtt.server_public_key_base64 must be padded RFC4648 base64")
        }
        if (decoded.size != 32) {
            throw AndroidClientProfileParseException("security.zero_rtt.server_public_key_base64 must decode to 32 bytes")
        }
    }
}

private fun JSONObject.requiredObject(path: String): JSONObject {
    val value = find(path) ?: throw AndroidClientProfileParseException("missing $path")
    return value as? JSONObject
        ?: throw AndroidClientProfileParseException("$path must be a JSON object")
}

private fun JSONObject.optionalObject(path: String): JSONObject? {
    val value = find(path) ?: return null
    return value as? JSONObject
        ?: throw AndroidClientProfileParseException("$path must be a JSON object")
}

private fun JSONObject.optionalArray(path: String): JSONArray? {
    val value = find(path) ?: return null
    return value as? JSONArray
        ?: throw AndroidClientProfileParseException("$path must be a JSON array")
}

private fun JSONObject.requiredString(path: String): String {
    val value = find(path) ?: throw AndroidClientProfileParseException("missing $path")
    return value as? String ?: throw AndroidClientProfileParseException("$path must be a string")
}

private fun JSONObject.requiredString(field: String, path: String): String {
    val value = optionalValue(field) ?: throw AndroidClientProfileParseException("missing $path")
    return value as? String ?: throw AndroidClientProfileParseException("$path must be a string")
}

private fun JSONObject.optionalString(field: String, path: String): String? {
    val value = optionalValue(field) ?: return null
    return value as? String ?: throw AndroidClientProfileParseException("$path must be a string")
}

private fun JSONObject.optionalBoolean(field: String, default: Boolean, path: String): Boolean {
    val value = optionalValue(field) ?: return default
    return value as? Boolean ?: throw AndroidClientProfileParseException("$path must be a boolean")
}

private fun JSONObject.optionalInt(field: String, default: Int, path: String): Int {
    val value = optionalValue(field) ?: return default
    val parsed = when (value) {
        is Int -> value
        is Long -> value.toIntIfExact(path)
        else -> throw AndroidClientProfileParseException("$path must be an integer")
    }
    if (parsed <= 0) {
        throw AndroidClientProfileParseException("$path must be positive")
    }
    return parsed
}

private fun JSONObject.optionalLong(field: String, default: Long, path: String): Long {
    val value = optionalValue(field) ?: return default
    val parsed = when (value) {
        is Int -> value.toLong()
        is Long -> value
        else -> throw AndroidClientProfileParseException("$path must be an integer")
    }
    if (parsed < 0) {
        throw AndroidClientProfileParseException("$path must be non-negative")
    }
    return parsed
}

private fun JSONObject.optionalPositiveLong(field: String, default: Long, path: String): Long {
    val parsed = optionalLong(field, default, path)
    if (parsed <= 0) {
        throw AndroidClientProfileParseException("$path must be positive")
    }
    return parsed
}

private fun JSONObject.find(path: String): Any? {
    var current: Any = this
    for (part in path.split('.')) {
        val objectValue = current as? JSONObject ?: return null
        current = objectValue.optionalValue(part) ?: return null
    }
    return current
}

private fun JSONObject.optionalValue(field: String): Any? {
    if (!has(field) || isNull(field)) {
        return null
    }
    return get(field)
}

private fun Long.toIntIfExact(path: String): Int {
    if (this < Int.MIN_VALUE || this > Int.MAX_VALUE) {
        throw AndroidClientProfileParseException("$path must fit into an integer")
    }
    return toInt()
}

private fun parseEndpoint(value: String, path: String): Endpoint {
    if (value.startsWith("[")) {
        val close = value.indexOf(']')
        if (close <= 1 || close + 2 > value.length || value[close + 1] != ':') {
            throw AndroidClientProfileParseException("$path must be HOST:PORT")
        }
        return endpointFromParts(value.substring(1, close), value.substring(close + 2), path)
    }
    val separator = value.lastIndexOf(':')
    if (separator <= 0 || separator == value.lastIndex) {
        throw AndroidClientProfileParseException("$path must be HOST:PORT")
    }
    val host = value.substring(0, separator)
    if (host.contains(':')) {
        throw AndroidClientProfileParseException("$path IPv6 addresses must use [addr]:port")
    }
    return endpointFromParts(host, value.substring(separator + 1), path)
}

private fun endpointFromParts(host: String, portText: String, path: String): Endpoint {
    val port = portText.toIntOrNull() ?: throw AndroidClientProfileParseException("$path port must be numeric")
    if (host.isEmpty() || port !in 1..65535) {
        throw AndroidClientProfileParseException("$path must be HOST:PORT")
    }
    return Endpoint(host, port)
}
