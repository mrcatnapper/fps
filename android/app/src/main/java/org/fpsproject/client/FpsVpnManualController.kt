package org.fpsproject.client

import android.content.Context
import android.content.Intent
import android.os.Build

internal enum class FpsVpnManualState {
    NO_PROFILE,
    READY,
    PROFILE_SAVED,
    START_REQUESTED,
    STOP_REQUESTED,
    PROFILE_CLEARED,
    ERROR,
}

internal data class FpsVpnManualSnapshot(
    val state: FpsVpnManualState,
    val profilePresent: Boolean,
    val message: String,
)

internal interface FpsVpnServiceCommandSender {
    fun startFromStoredProfile()

    fun stop()
}

internal class AndroidFpsVpnServiceCommandSender(
    private val context: Context,
) : FpsVpnServiceCommandSender {
    override fun startFromStoredProfile() {
        val intent = Intent(context, FpsVpnService::class.java).apply {
            action = FpsVpnService.ACTION_START
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent)
        } else {
            context.startService(intent)
        }
    }

    override fun stop() {
        context.startService(Intent(context, FpsVpnService::class.java).apply {
            action = FpsVpnService.ACTION_STOP
        })
    }
}

internal class FpsVpnManualController(
    private val repository: FpsVpnProfileRepository,
    private val commandSender: FpsVpnServiceCommandSender,
) {
    private var lastSnapshot: FpsVpnManualSnapshot = baseSnapshot()

    fun saveProfile(profileText: String): FpsVpnManualSnapshot {
        return try {
            repository.saveProfile(profileText)
            update(FpsVpnManualState.PROFILE_SAVED, "Profile saved")
        } catch (_: IllegalArgumentException) {
            update(FpsVpnManualState.ERROR, "profile_invalid")
        }
    }

    fun start(): FpsVpnManualSnapshot {
        val profile = try {
            repository.loadProfile()
        } catch (_: IllegalArgumentException) {
            return update(FpsVpnManualState.ERROR, "profile_invalid")
        }
        if (profile == null) {
            return update(FpsVpnManualState.ERROR, "profile_missing")
        }
        commandSender.startFromStoredProfile()
        return update(FpsVpnManualState.START_REQUESTED, "Start requested")
    }

    fun stop(): FpsVpnManualSnapshot {
        commandSender.stop()
        return update(FpsVpnManualState.STOP_REQUESTED, "Stop requested")
    }

    fun clear(): FpsVpnManualSnapshot {
        commandSender.stop()
        repository.clearProfile()
        return update(FpsVpnManualState.PROFILE_CLEARED, "Profile cleared")
    }

    fun permissionDenied(): FpsVpnManualSnapshot {
        return update(FpsVpnManualState.ERROR, "vpn_permission_denied")
    }

    fun refresh(): FpsVpnManualSnapshot {
        lastSnapshot = baseSnapshot()
        return lastSnapshot
    }

    fun snapshot(): FpsVpnManualSnapshot = lastSnapshot

    private fun update(state: FpsVpnManualState, message: String): FpsVpnManualSnapshot {
        lastSnapshot = FpsVpnManualSnapshot(
            state = state,
            profilePresent = repository.hasProfile(),
            message = message,
        )
        return lastSnapshot
    }

    private fun baseSnapshot(): FpsVpnManualSnapshot {
        val hasProfile = repository.hasProfile()
        if (hasProfile) {
            try {
                repository.loadProfile()
            } catch (_: IllegalArgumentException) {
                return FpsVpnManualSnapshot(
                    state = FpsVpnManualState.ERROR,
                    profilePresent = true,
                    message = "profile_invalid",
                )
            }
        }
        return FpsVpnManualSnapshot(
            state = if (hasProfile) FpsVpnManualState.READY else FpsVpnManualState.NO_PROFILE,
            profilePresent = hasProfile,
            message = if (hasProfile) "Profile ready" else "No profile saved",
        )
    }
}
