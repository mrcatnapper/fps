package org.fpsproject.client

import org.fpsproject.client.config.AndroidClientProfileParseException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.charset.StandardCharsets
import java.util.Base64

class FpsVpnProfileRepositoryTest {
    @Test
    fun savesLoadsAndClearsJsonProfile() {
        val storage = MapProfileStorage()
        val repository = ValidatingFpsVpnProfileRepository(storage)

        val normalized = repository.saveProfile("  $PROFILE_JSON  ")

        assertEquals(PROFILE_JSON, normalized)
        assertEquals(PROFILE_JSON, storage.value)
        assertEquals(PROFILE_JSON, repository.loadProfile())
        assertTrue(repository.hasProfile())

        repository.clearProfile()

        assertNull(repository.loadProfile())
        assertFalse(repository.hasProfile())
    }

    @Test
    fun acceptsFpsUriAndStoresDecodedJson() {
        val repository = ValidatingFpsVpnProfileRepository(MapProfileStorage())

        val normalized = repository.saveProfile(profileUri(PROFILE_JSON))

        assertEquals(PROFILE_JSON, normalized)
        assertEquals(PROFILE_JSON, repository.loadProfile())
    }

    @Test
    fun rejectsInvalidProfileWithoutOverwritingExistingProfile() {
        val storage = MapProfileStorage()
        val repository = ValidatingFpsVpnProfileRepository(storage)

        repository.saveProfile(PROFILE_JSON)
        assertThrows(AndroidClientProfileParseException::class.java) {
            repository.saveProfile("""{"network":{"server":"fps.example.test:443"}}""")
        }

        assertEquals(PROFILE_JSON, repository.loadProfile())
    }

    @Test
    fun rejectsCorruptedStoredProfileOnLoad() {
        val repository = ValidatingFpsVpnProfileRepository(MapProfileStorage("""{"broken":true}"""))

        assertThrows(AndroidClientProfileParseException::class.java) {
            repository.loadProfile()
        }
    }

    @Test
    fun repositoryStringsDoNotExposeStoredSecretMaterial() {
        val storage = MapProfileStorage()
        val repository = ValidatingFpsVpnProfileRepository(storage)

        repository.saveProfile(PROFILE_JSON)
        val text = repository.toString() + storage.toString()

        assertFalse(text.contains(CLIENT_UUID))
        assertFalse(text.contains(SERVER_KEY_BASE64))
    }

    private companion object {
        private const val CLIENT_UUID = "123e4567-e89b-42d3-a456-426614174000"
        private const val SERVER_KEY_BASE64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
        private val PROFILE_JSON = """
            {"network":{"server":"fps.example.test:443"},"security":{"zero_rtt":{"profile_id":"android-test-v5","client_uuid":"$CLIENT_UUID","server_public_key_base64":"$SERVER_KEY_BASE64"}}}
        """.trimIndent()

        private fun profileUri(json: String): String {
            val encoded = Base64.getUrlEncoder()
                .withoutPadding()
                .encodeToString(json.toByteArray(StandardCharsets.UTF_8))
            return "fps://v1/$encoded"
        }
    }
}

private class MapProfileStorage(
    var value: String? = null,
) : FpsVpnProfileStorage {
    override fun read(): String? = value

    override fun write(value: String) {
        this.value = value
    }

    override fun clear() {
        value = null
    }

    override fun toString(): String = "MapProfileStorage(value=<redacted>)"
}
