package org.fpsproject.client

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.net.VpnService
import android.os.Bundle
import android.text.InputType
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
    private lateinit var controller: FpsVpnManualController
    private lateinit var profileInput: EditText
    private lateinit var statusText: TextView
    private var startAfterPermission = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        controller = FpsVpnManualController(
            repository = SharedPreferencesFpsVpnProfileRepository(this),
            commandSender = AndroidFpsVpnServiceCommandSender(this),
        )
        setContentView(buildContentView())
        render(controller.refresh())
    }

    @Deprecated("Deprecated in Android framework API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_VPN_PERMISSION || !startAfterPermission) {
            return
        }
        startAfterPermission = false
        if (resultCode == RESULT_OK) {
            render(controller.start())
        } else {
            render(controller.permissionDenied())
        }
    }

    private fun buildContentView(): ScrollView {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        root.addView(TextView(this).apply {
            text = "FPS Android"
            textSize = 22f
        })
        profileInput = EditText(this).apply {
            hint = "Paste fps://v1 URI or client JSON profile"
            minLines = 8
            maxLines = 16
            typeface = Typeface.MONOSPACE
            inputType = InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
        root.addView(profileInput, matchWidthWrapHeight())
        root.addView(button("Save Profile") { render(controller.saveProfile(profileInput.text.toString())) })
        root.addView(button("Start VPN") { startVpnWithPermission() })
        root.addView(button("Stop VPN") { render(controller.stop()) })
        root.addView(button("Clear Profile") {
            profileInput.text.clear()
            render(controller.clear())
        })
        root.addView(button("Refresh Status") { render(controller.refresh()) })
        statusText = TextView(this).apply {
            textSize = 14f
            typeface = Typeface.MONOSPACE
        }
        root.addView(statusText, matchWidthWrapHeight())
        return ScrollView(this).apply {
            addView(root, matchWidthWrapHeight())
        }
    }

    private fun button(label: String, onClick: () -> Unit): Button {
        return Button(this).apply {
            text = label
            setOnClickListener { onClick() }
        }
    }

    private fun startVpnWithPermission() {
        val current = controller.refresh()
        if (!current.profilePresent || current.state == FpsVpnManualState.ERROR) {
            render(current)
            return
        }
        val prepareIntent = VpnService.prepare(this)
        if (prepareIntent != null) {
            startAfterPermission = true
            startActivityForResult(prepareIntent, REQUEST_VPN_PERMISSION)
            statusText.text = "state=STARTING\nprofile_saved=${controller.snapshot().profilePresent}\nmessage=vpn_permission_requested"
            return
        }
        render(controller.start())
    }

    private fun render(snapshot: FpsVpnManualSnapshot) {
        statusText.text = "state=${snapshot.state}\nprofile_saved=${snapshot.profilePresent}\nmessage=${snapshot.message}"
    }

    private fun matchWidthWrapHeight(): LinearLayout.LayoutParams {
        return LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        )
    }

    private companion object {
        private const val REQUEST_VPN_PERMISSION = 6602
    }
}
