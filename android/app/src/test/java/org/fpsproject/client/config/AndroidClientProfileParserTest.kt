package org.fpsproject.client.config

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.charset.StandardCharsets
import java.util.Base64

class AndroidClientProfileParserTest {
    private val key = Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })
    private val uuid = "123e4567-e89b-42d3-a456-426614174000"

    private fun profileJson(
        extraZeroRtt: String = "",
        extraTun: String = "",
        carriers: String = """
          "carriers": [
            {"mode": "https_get", "endpoint": "origin.example.test:443", "path": "/ping", "interval_ms": 5000},
            {"mode": "wss", "endpoint": "[2001:db8::1]:9443", "path": "/stream", "interval_ms": 10000, "max_response_bytes": 2048}
          ],
        """.trimIndent(),
        splitTunnel: String = """
          "split_tunnel": {
            "allowed_uids": [10042, 10043]
          },
        """.trimIndent(),
    ) = """
        {
          "network": {
            "server": "fps.example.test:443",
            "read_buffer_size": 65536
          },
          "security": {
            "zero_rtt": {
              "enabled": true,
              "profile_id": "android-test-v5",
              "client_uuid": "$uuid",
              "server_public_key_base64": "$key",
              "client_upgrade_delay_ms": 2000
              $extraZeroRtt
            }
          },
          "codec": {
            "max_frame_payload": 1280,
            "max_frame_padding": 64,
            "allow_fragmentation": true
          },
          "tun": {
            "enabled": true,
            "name": "fpsc0",
            "mtu": 1280,
            "auto_configure": true
            $extraTun
          },
          "ops": {
            "status_socket": "/run/fps/client.status"
          },
          $carriers
          $splitTunnel
          "unused_tail": true
        }
    """.trimIndent()

    @Test
    fun parsesClientJsonProfile() {
        val profile = AndroidClientProfileParser.parse(profileJson())

        assertEquals("fps.example.test", profile.server.host)
        assertEquals(443, profile.server.port)
        assertEquals("android-test-v5", profile.zeroRtt.profileId)
        assertEquals(1280, profile.codec.maxFramePayload)
        assertEquals(1280, profile.tun?.mtu)
        assertEquals("/run/fps/client.status", profile.ops.statusSocket)
        assertEquals(2, profile.carriers.size)
        assertEquals(CarrierProbeMode.HTTPS_GET, profile.carriers[0].mode)
        assertEquals("origin.example.test", profile.carriers[0].endpoint.host)
        assertEquals("/stream", profile.carriers[1].path)
        assertEquals(DEFAULT_MAX_CARRIER_RESPONSE_BYTES, profile.carriers[0].maxResponseBytes)
        assertEquals(2048, profile.carriers[1].maxResponseBytes)
        assertEquals(setOf(10042, 10043), profile.splitTunnel.allowedUids)
    }

    @Test
    fun parsesFpsUriProfile() {
        val encoded = Base64.getUrlEncoder().withoutPadding().encodeToString(profileJson().toByteArray(StandardCharsets.UTF_8))
        val uri = "fps://v1/$encoded"

        val profile = AndroidClientProfileParser.parse(uri)

        assertEquals("fps.example.test", profile.server.host)
        assertEquals(uuid, profile.zeroRtt.clientUuid)
    }

    @Test
    fun rejectsMissingRequiredFields() {
        val error = assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse("""{"security":{"zero_rtt":{}}}""")
        }

        assertTrue(error.message!!.contains("network.server"))
    }

    @Test
    fun rejectsMalformedUuidAndWrongKeyLength() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson().replace(uuid, "not-a-uuid"))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson().replace(key, Base64.getEncoder().encodeToString(ByteArray(31))))
        }
    }

    @Test
    fun rejectsDisabledZeroRttProfile() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson().replace("\"enabled\": true", "\"enabled\": false"))
        }
    }

    @Test
    fun rejectsServerOnlyFields() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(extraZeroRtt = ",\"server_private_key_base64\":\"$key\""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(extraZeroRtt = ",\"allowed_client_uuids\":[\"$uuid\"]"))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(extraTun = ",\"lease_pool\":\"10.66.0.0/30\""))
        }
    }

    @Test
    fun doesNotLeakSecretFieldsInToString() {
        val profile = AndroidClientProfileParser.parse(profileJson())
        val text = profile.toString()

        assertFalse(text.contains(uuid))
        assertFalse(text.contains(key))
        assertFalse(text.contains("10042"))
        assertTrue(text.contains("<redacted>"))
        assertTrue(text.contains("allowedUidCount=2"))
    }

    @Test
    fun carrierProbeProfileValidatesShape() {
        val probe = CarrierProbeProfile(
            mode = CarrierProbeMode.HTTPS_GET,
            endpoint = Endpoint("origin.example.test", 443),
            path = "/ping",
            intervalMs = 10_000,
            maxResponseBytes = 4096,
        )

        assertEquals(CarrierProbeMode.HTTPS_GET, probe.mode)
        assertEquals(4096, probe.maxResponseBytes)
        assertThrows(IllegalArgumentException::class.java) {
            CarrierProbeProfile(CarrierProbeMode.WSS, Endpoint("origin.example.test", 443), "relative", 10_000)
        }
        assertThrows(IllegalArgumentException::class.java) {
            CarrierProbeProfile(CarrierProbeMode.WSS, Endpoint("origin.example.test", 443), "/", 0)
        }
        assertThrows(IllegalArgumentException::class.java) {
            CarrierProbeProfile(CarrierProbeMode.WSS, Endpoint("origin.example.test", 443), "/", 10_000, 0)
        }
    }

    @Test
    fun rejectsInvalidCarrierConfig() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(carriers = """"carriers": [{"mode": "tcp", "endpoint": "origin.example.test:443"}],"""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(carriers = """"carriers": [{"mode": "wss", "endpoint": "origin.example.test", "path": "/"}],"""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(carriers = """"carriers": [{"mode": "https_get", "endpoint": "origin.example.test:443", "path": "relative"}],"""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(carriers = """"carriers": [{"mode": "https_get", "endpoint": "origin.example.test:443", "interval_ms": 0}],"""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(carriers = """"carriers": [{"mode": "https_get", "endpoint": "origin.example.test:443", "max_response_bytes": 0}],"""))
        }
    }

    @Test
    fun parsesMissingCarrierAndSplitTunnelAsEmpty() {
        val profile = AndroidClientProfileParser.parse(profileJson(carriers = "", splitTunnel = ""))

        assertEquals(emptyList<CarrierProbeProfile>(), profile.carriers)
        assertEquals(emptySet<Int>(), profile.splitTunnel.allowedUids)
    }

    @Test
    fun rejectsInvalidSplitTunnelAllowlist() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(splitTunnel = """"split_tunnel": {"allowed_uids": [-1]},"""))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(splitTunnel = """"split_tunnel": {"allowed_uids": ["10042"]},"""))
        }
    }
}
