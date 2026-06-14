package org.fpsproject.client

import android.content.Context
import org.fpsproject.client.config.AndroidClientProfileParser

internal interface FpsVpnProfileRepository {
    fun saveProfile(profileText: String): String

    fun loadProfile(): String?

    fun clearProfile()

    fun hasProfile(): Boolean
}

internal interface FpsVpnProfileStorage {
    fun read(): String?

    fun write(value: String)

    fun clear()
}

internal class ValidatingFpsVpnProfileRepository(
    private val storage: FpsVpnProfileStorage,
) : FpsVpnProfileRepository {
    override fun saveProfile(profileText: String): String {
        val normalized = AndroidClientProfileParser.normalizeJsonText(profileText)
        storage.write(normalized)
        return normalized
    }

    override fun loadProfile(): String? {
        val stored = storage.read() ?: return null
        return AndroidClientProfileParser.normalizeJsonText(stored)
    }

    override fun clearProfile() {
        storage.clear()
    }

    override fun hasProfile(): Boolean = storage.read() != null
}

internal class SharedPreferencesFpsVpnProfileRepository(
    context: Context,
) : FpsVpnProfileRepository {
    private val delegate = ValidatingFpsVpnProfileRepository(
        SharedPreferencesFpsVpnProfileStorage(context.applicationContext),
    )

    override fun saveProfile(profileText: String): String = delegate.saveProfile(profileText)

    override fun loadProfile(): String? = delegate.loadProfile()

    override fun clearProfile() {
        delegate.clearProfile()
    }

    override fun hasProfile(): Boolean = delegate.hasProfile()
}

private class SharedPreferencesFpsVpnProfileStorage(
    context: Context,
) : FpsVpnProfileStorage {
    private val preferences = context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    override fun read(): String? = preferences.getString(KEY_PROFILE, null)

    override fun write(value: String) {
        preferences.edit().putString(KEY_PROFILE, value).apply()
    }

    override fun clear() {
        preferences.edit().remove(KEY_PROFILE).apply()
    }

    private companion object {
        private const val PREFERENCES_NAME = "fps_vpn_profile"
        private const val KEY_PROFILE = "profile_text"
    }
}
