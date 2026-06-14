package org.fpsproject.client

import android.content.Context
import android.content.Intent
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.Until
import org.junit.Assert.assertNotNull
import org.junit.Test
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

    private fun clearStoredState() {
        targetContext.getSharedPreferences("fps_vpn_profile", Context.MODE_PRIVATE)
            .edit()
            .clear()
            .commit()
        SharedPreferencesFpsVpnStatusStore(targetContext).clear()
    }
}
