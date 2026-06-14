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
        shaper: String = "",
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
          $shaper
          $splitTunnel
          "unused_tail": true
        }
    """.trimIndent()

    private fun shaperJson(
        extra: String = "",
        recordSizeC2s: String = "[[512, 0.5], [1500, 1.0]]",
        recordSizeS2c: String = "[[640, 0.25], [1510, 1.0]]",
        delayC2s: String = "[[1000, 0.7], [10000, 1.0]]",
        delayS2c: String = "[[2000, 0.5], [15000, 1.0]]",
    ) = """
          "shaper": {
            "enabled": true,
            "profile_id": "android-shaper-test",
            "record_size_cdf_c2s": $recordSizeC2s,
            "record_size_cdf_s2c": $recordSizeS2c,
            "inter_record_delay_us_cdf_c2s": $delayC2s,
            "inter_record_delay_us_cdf_s2c": $delayS2c,
            "covert_ratio_max": 0.25,
            "burst_records_max": 2,
            "jitter_ms": {"min": 1, "max": 3},
            "adaptive": {
              "enabled": false,
              "min_records": 4,
              "min_observation_ms": 500,
              "decay": 0.75,
              "snapshot_interval_ms": 1000
            },
            "deterministic_seed": "42"
            $extra
          },
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
        assertEquals(null, profile.shaper)
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
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = """"shaper": {"enabled": true, "profile_file": "profile.json"},"""))
        }
    }

    @Test
    fun parsesInlineShaperProfile() {
        val profile = AndroidClientProfileParser.parse(profileJson(shaper = shaperJson()))
        val shaper = profile.shaper!!

        assertEquals("android-shaper-test", shaper.profileId)
        assertEquals(listOf(CdfPointProfile(512L, 0.5), CdfPointProfile(1500L, 1.0)), shaper.clientToServer.recordSizeCdf)
        assertEquals(listOf(CdfPointProfile(640L, 0.25), CdfPointProfile(1510L, 1.0)), shaper.serverToClient.recordSizeCdf)
        assertEquals(listOf(CdfPointProfile(1000L, 0.7), CdfPointProfile(10000L, 1.0)), shaper.clientToServer.interRecordDelayUsCdf)
        assertEquals(0.25, shaper.covertRatioMax, 0.0)
        assertEquals(2, shaper.burstRecordsMax)
        assertEquals(1L, shaper.jitterMinMs)
        assertEquals(3L, shaper.jitterMaxMs)
        assertFalse(shaper.adaptive.enabled)
        assertEquals(4, shaper.adaptive.minRecords)
        assertEquals(500L, shaper.adaptive.minObservationMs)
        assertEquals(0.75, shaper.adaptive.decay, 0.0)
        assertEquals(1000L, shaper.adaptive.snapshotIntervalMs)
        assertEquals(42L, shaper.deterministicSeed)
    }

    @Test
    fun rejectsInvalidShaperProfiles() {
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson(recordSizeC2s = "[]")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson(recordSizeC2s = """[{"le": 512, "p": 1.0}]""")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson(recordSizeC2s = "[[1500, 0.5], [512, 1.0]]")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson(recordSizeC2s = "[[512, 0.5], [1500, 0.9]]")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson(recordSizeC2s = "[[0, 1.0]]")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson().replace(""""deterministic_seed": "42"""", """"deterministic_seed": -1""")))
        }
        assertThrows(AndroidClientProfileParseException::class.java) {
            AndroidClientProfileParser.parse(profileJson(shaper = shaperJson().replace(""""decay": 0.75""", """"decay": 0.0""")))
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
