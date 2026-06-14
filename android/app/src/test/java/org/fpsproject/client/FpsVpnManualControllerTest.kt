package org.fpsproject.client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test
import java.nio.charset.StandardCharsets
import java.util.Base64

class FpsVpnManualControllerTest {
    @Test
    fun initialSnapshotReportsMissingOrSavedProfile() {
        val empty = controllerWithStorage()
        val saved = controllerWithStorage(PROFILE_JSON)
        val corrupted = controllerWithStorage("""{"broken":true}""")

        assertEquals(FpsVpnManualState.NO_PROFILE, empty.controller.snapshot().state)
        assertFalse(empty.controller.snapshot().profilePresent)
        assertEquals(FpsVpnManualState.READY, saved.controller.snapshot().state)
        assertEquals(true, saved.controller.snapshot().profilePresent)
        assertEquals(FpsVpnManualState.ERROR, corrupted.controller.snapshot().state)
        assertEquals("profile_invalid", corrupted.controller.snapshot().message)
    }

    @Test
    fun saveProfileAcceptsJsonAndFpsUri() {
        val fixture = controllerWithStorage()

        val savedJson = fixture.controller.saveProfile("  $PROFILE_JSON  ")
        val savedUri = fixture.controller.saveProfile(profileUri(PROFILE_JSON))

        assertEquals(FpsVpnManualState.PROFILE_SAVED, savedJson.state)
        assertEquals(FpsVpnManualState.PROFILE_SAVED, savedUri.state)
        assertEquals(PROFILE_JSON, fixture.storage.value)
        assertEquals(emptyList<String>(), fixture.commands.commands)
    }

    @Test
    fun invalidProfileDoesNotOverwriteOrStart() {
        val fixture = controllerWithStorage(PROFILE_JSON)

        val snapshot = fixture.controller.saveProfile("""{"broken":true}""")

        assertEquals(FpsVpnManualState.ERROR, snapshot.state)
        assertEquals("profile_invalid", snapshot.message)
        assertEquals(PROFILE_JSON, fixture.storage.value)
        assertEquals(emptyList<String>(), fixture.commands.commands)
    }

    @Test
    fun startRequiresValidStoredProfileAndSendsStartCommandWithoutProfileText() {
        val fixture = controllerWithStorage(PROFILE_JSON)

        val snapshot = fixture.controller.start()

        assertEquals(FpsVpnManualState.START_REQUESTED, snapshot.state)
        assertEquals(listOf("start"), fixture.commands.commands)
        val commandText = fixture.commands.commands.toString()
        assertFalse(commandText.contains(PROFILE_JSON))
        assertFalse(commandText.contains(CLIENT_UUID))
        assertFalse(commandText.contains(SERVER_KEY_BASE64))
    }

    @Test
    fun startWithoutProfileReportsMissingProfile() {
        val fixture = controllerWithStorage()

        val snapshot = fixture.controller.start()

        assertEquals(FpsVpnManualState.ERROR, snapshot.state)
        assertEquals("profile_missing", snapshot.message)
        assertEquals(emptyList<String>(), fixture.commands.commands)
    }

    @Test
    fun startWithCorruptedStoredProfileReportsInvalidProfile() {
        val fixture = controllerWithStorage("""{"broken":true}""")

        val snapshot = fixture.controller.start()

        assertEquals(FpsVpnManualState.ERROR, snapshot.state)
        assertEquals("profile_invalid", snapshot.message)
        assertEquals(emptyList<String>(), fixture.commands.commands)
    }

    @Test
    fun stopSendsStopWithoutClearingProfile() {
        val fixture = controllerWithStorage(PROFILE_JSON)

        val snapshot = fixture.controller.stop()

        assertEquals(FpsVpnManualState.STOP_REQUESTED, snapshot.state)
        assertEquals(PROFILE_JSON, fixture.storage.value)
        assertEquals(listOf("stop"), fixture.commands.commands)
    }

    @Test
    fun clearStopsAndDeletesProfile() {
        val fixture = controllerWithStorage(PROFILE_JSON)

        val snapshot = fixture.controller.clear()

        assertEquals(FpsVpnManualState.PROFILE_CLEARED, snapshot.state)
        assertEquals(null, fixture.storage.value)
        assertEquals(listOf("stop"), fixture.commands.commands)
    }

    @Test
    fun permissionDeniedReturnsSafeErrorWithoutSendingCommand() {
        val fixture = controllerWithStorage(PROFILE_JSON)

        val snapshot = fixture.controller.permissionDenied()

        assertEquals(FpsVpnManualState.ERROR, snapshot.state)
        assertEquals("vpn_permission_denied", snapshot.message)
        assertEquals(emptyList<String>(), fixture.commands.commands)
    }

    @Test
    fun snapshotStringsDoNotExposeSecretMaterial() {
        val fixture = controllerWithStorage()

        val text = fixture.controller.saveProfile(PROFILE_JSON).toString() +
            fixture.controller.start().toString() +
            fixture.controller.snapshot().toString()

        assertFalse(text.contains(PROFILE_JSON))
        assertFalse(text.contains(CLIENT_UUID))
        assertFalse(text.contains(SERVER_KEY_BASE64))
    }

    private fun controllerWithStorage(value: String? = null): ControllerFixture {
        val storage = ManualControllerProfileStorage(value)
        val commands = RecordingCommandSender()
        val controller = FpsVpnManualController(
            repository = ValidatingFpsVpnProfileRepository(storage),
            commandSender = commands,
        )
        return ControllerFixture(controller, storage, commands)
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

private data class ControllerFixture(
    val controller: FpsVpnManualController,
    val storage: ManualControllerProfileStorage,
    val commands: RecordingCommandSender,
)

private class ManualControllerProfileStorage(
    var value: String? = null,
) : FpsVpnProfileStorage {
    override fun read(): String? = value

    override fun write(value: String) {
        this.value = value
    }

    override fun clear() {
        value = null
    }
}

private class RecordingCommandSender : FpsVpnServiceCommandSender {
    val commands = mutableListOf<String>()

    override fun startFromStoredProfile() {
        commands += "start"
    }

    override fun stop() {
        commands += "stop"
    }
}
