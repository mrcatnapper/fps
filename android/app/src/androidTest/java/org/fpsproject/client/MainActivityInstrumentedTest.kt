package org.fpsproject.client

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.Until
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.charset.StandardCharsets
import java.util.Base64
import java.util.regex.Pattern

class MainActivityInstrumentedTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val targetContext: Context = instrumentation.targetContext

    @Test
    fun launcherStartsAndReportsNoStoredProfile() {
        clearStoredState()

        val launchIntent = targetContext.packageManager.getLaunchIntentForPackage(targetContext.packageName)
        assertNotNull("Launcher intent is missing", launchIntent)
        targetContext.startActivity(launchIntent!!.apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        })

        val device = UiDevice.getInstance(instrumentation)
        assertNotNull("Main activity title was not visible", device.wait(Until.findObject(By.text("FPS Android")), 10_000))
        assertNotNull(
            "Save button was not visible",
            device.wait(Until.findObject(By.text(Pattern.compile("(?i)Save Profile"))), 5_000),
        )
        assertNotNull(
            "Initial no-profile status was not visible",
            device.wait(Until.findObject(By.textContains("profile_state=NO_PROFILE")), 5_000),
        )
        assertNotNull(
            "Initial stopped VPN status was not visible",
            device.wait(Until.findObject(By.textContains("vpn_state=STOPPED")), 5_000),
        )
        assertNotNull(
            "Refresh label should describe combined status",
            device.wait(Until.findObject(By.text(Pattern.compile("(?i)Refresh Status"))), 5_000),
        )
    }

    @Test
    fun profileDeepLinkPrefillsThenSavesOnlyAfterUserAction() {
        clearStoredState()

        launchProfileUri(profileUri(PROFILE_JSON))

        val device = UiDevice.getInstance(instrumentation)
        assertNotNull("Main activity title was not visible", device.wait(Until.findObject(By.text("FPS Android")), 10_000))
        assertNotNull(
            "Deep-link import status was not visible",
            device.wait(Until.findObject(By.textContains("profile_message=profile_import_ready")), 5_000),
        )
        assertEquals(null, storedProfileText())

        val saveButton = device.wait(Until.findObject(By.text(Pattern.compile("(?i)Save Profile"))), 5_000)
        assertNotNull("Save button was not visible", saveButton)
        saveButton!!.click()

        assertNotNull(
            "Saved profile status was not visible",
            device.wait(Until.findObject(By.textContains("profile_state=PROFILE_SAVED")), 5_000),
        )
        assertEquals(PROFILE_JSON, storedProfileText())
    }

    @Test
    fun invalidProfileDeepLinkDoesNotOverwriteSavedProfile() {
        clearStoredState()
        targetContext.getSharedPreferences(PROFILE_PREFERENCES_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PROFILE_PREFERENCES_KEY, PROFILE_JSON)
            .commit()

        launchProfileUri("fps://v1/not@base64")

        val device = UiDevice.getInstance(instrumentation)
        assertNotNull("Main activity title was not visible", device.wait(Until.findObject(By.text("FPS Android")), 10_000))
        assertNotNull(
            "Invalid deep-link import status was not visible",
            device.wait(Until.findObject(By.textContains("profile_message=profile_import_invalid")), 5_000),
        )
        assertEquals(PROFILE_JSON, storedProfileText())
    }

    private fun clearStoredState() {
        targetContext.getSharedPreferences(PROFILE_PREFERENCES_NAME, Context.MODE_PRIVATE)
            .edit()
            .clear()
            .commit()
        SharedPreferencesFpsVpnStatusStore(targetContext).clear()
    }

    private fun launchProfileUri(uri: String) {
        targetContext.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(uri)).apply {
            setPackage(targetContext.packageName)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        })
    }

    private fun storedProfileText(): String? {
        return targetContext.getSharedPreferences(PROFILE_PREFERENCES_NAME, Context.MODE_PRIVATE)
            .getString(PROFILE_PREFERENCES_KEY, null)
    }

    private fun profileUri(json: String): String {
        val encoded = Base64.getUrlEncoder()
            .withoutPadding()
            .encodeToString(json.toByteArray(StandardCharsets.UTF_8))
        return "fps://v1/$encoded"
    }

    private companion object {
        private const val PROFILE_PREFERENCES_NAME = "fps_vpn_profile"
        private const val PROFILE_PREFERENCES_KEY = "profile_text"
        private const val CLIENT_UUID = "123e4567-e89b-42d3-a456-426614174000"
        private const val SERVER_KEY_BASE64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
        private val PROFILE_JSON = """
            {"network":{"server":"fps.example.test:443"},"security":{"zero_rtt":{"profile_id":"android-test-v5","client_uuid":"$CLIENT_UUID","server_public_key_base64":"$SERVER_KEY_BASE64"}}}
        """.trimIndent()
    }
}
