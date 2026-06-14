package org.fpsproject.client

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.pm.ServiceInfo
import android.os.Build
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerState

internal enum class FpsVpnStatusState {
    STARTING,
    WAITING_FOR_LEASE,
    RUNNING,
    BACKOFF,
    FAILED,
    STOPPED,
}

internal data class FpsVpnStatusSnapshot(
    val state: FpsVpnStatusState,
    val attempts: Int = 0,
    val reconnects: Int = 0,
    val nextRetryDelayMs: Long = 0,
    val error: String? = null,
) {
    companion object {
        fun starting() = FpsVpnStatusSnapshot(state = FpsVpnStatusState.STARTING)

        fun failed(error: String) = FpsVpnStatusSnapshot(
            state = FpsVpnStatusState.FAILED,
            error = safeErrorName(error),
        )

        fun stopped() = FpsVpnStatusSnapshot(state = FpsVpnStatusState.STOPPED)

        fun fromRunner(snapshot: CoordinatedNativeVpnRunnerSnapshot): FpsVpnStatusSnapshot {
            val state = when (snapshot.state) {
                CoordinatedNativeVpnRunnerState.STOPPED -> FpsVpnStatusState.STOPPED
                CoordinatedNativeVpnRunnerState.STARTING -> FpsVpnStatusState.STARTING
                CoordinatedNativeVpnRunnerState.WAITING_FOR_LEASE -> FpsVpnStatusState.WAITING_FOR_LEASE
                CoordinatedNativeVpnRunnerState.RUNNING -> FpsVpnStatusState.RUNNING
                CoordinatedNativeVpnRunnerState.BACKOFF -> FpsVpnStatusState.BACKOFF
                CoordinatedNativeVpnRunnerState.NEEDS_VPN_PERMISSION,
                CoordinatedNativeVpnRunnerState.FAILED,
                -> FpsVpnStatusState.FAILED
            }
            val error = if (state == FpsVpnStatusState.STOPPED) {
                null
            } else {
                safeErrorName(
                    snapshot.lastError
                        ?: snapshot.runtime.vpn.lastError
                        ?: snapshot.runtime.native.lastError,
                )
            }
            return FpsVpnStatusSnapshot(
                state = state,
                attempts = snapshot.attempts,
                reconnects = snapshot.reconnects,
                nextRetryDelayMs = snapshot.nextRetryDelayMs,
                error = error,
            )
        }
    }
}

internal interface FpsVpnStatusNotifier {
    fun show(snapshot: FpsVpnStatusSnapshot)

    fun clear()
}

internal object NoopFpsVpnStatusNotifier : FpsVpnStatusNotifier {
    override fun show(snapshot: FpsVpnStatusSnapshot) = Unit

    override fun clear() = Unit
}

internal class AndroidFpsVpnStatusNotifier(
    private val service: Service,
) : FpsVpnStatusNotifier {
    override fun show(snapshot: FpsVpnStatusSnapshot) {
        createChannel()
        val notification = buildNotification(snapshot)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            service.startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            service.startForeground(NOTIFICATION_ID, notification)
        }
    }

    override fun clear() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            service.stopForeground(Service.STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            service.stopForeground(true)
        }
    }

    private fun createChannel() {
        val manager = service.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val existing = manager.getNotificationChannel(CHANNEL_ID)
        if (existing == null) {
            val channel = NotificationChannel(CHANNEL_ID, "FPS VPN status", NotificationManager.IMPORTANCE_LOW)
            channel.description = "FPS VPN runtime state"
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(snapshot: FpsVpnStatusSnapshot): Notification {
        return Notification.Builder(service, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_fps_notification)
            .setContentTitle("FPS VPN")
            .setContentText(notificationText(snapshot))
            .setCategory(Notification.CATEGORY_SERVICE)
            .setOngoing(snapshot.state != FpsVpnStatusState.FAILED)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .build()
    }

    private fun notificationText(snapshot: FpsVpnStatusSnapshot): String {
        return when (snapshot.state) {
            FpsVpnStatusState.STARTING -> "Starting"
            FpsVpnStatusState.WAITING_FOR_LEASE -> "Waiting for server lease"
            FpsVpnStatusState.RUNNING -> "Running"
            FpsVpnStatusState.BACKOFF -> {
                val seconds = (snapshot.nextRetryDelayMs + 999) / 1000
                "Reconnecting in ${seconds}s"
            }
            FpsVpnStatusState.FAILED -> "Stopped: ${snapshot.error ?: "failed"}"
            FpsVpnStatusState.STOPPED -> "Stopped"
        }
    }

    private companion object {
        private const val CHANNEL_ID = "fps_vpn_status"
        private const val NOTIFICATION_ID = 6601
    }
}

private val uuidRegex = Regex(
    "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}",
)
private val safeErrorRegex = Regex("[a-z0-9_:-]{1,64}")

private fun safeErrorName(value: String?): String? {
    val candidate = value ?: return null
    if (uuidRegex.containsMatchIn(candidate)) {
        return "error"
    }
    return candidate.takeIf { safeErrorRegex.matches(it) } ?: "error"
}
