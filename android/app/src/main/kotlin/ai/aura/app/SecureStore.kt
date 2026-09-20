package ai.aura.app

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import java.util.UUID

/**
 * Хранилище токенов и адреса сервера — EncryptedSharedPreferences
 * (ключи в Android Keystore, аналог Keychain на iOS). В обычном
 * SharedPreferences остаётся только нечувствительный device_id.
 */
class SecureStore(context: Context) {

    private val masterKey = MasterKey.Builder(context)
        .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
        .build()

    private val secure: SharedPreferences = EncryptedSharedPreferences.create(
        context,
        "aura-secure",
        masterKey,
        EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
        EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
    )

    private val plain: SharedPreferences =
        context.getSharedPreferences("aura-device", Context.MODE_PRIVATE)

    var serverUrl: String
        get() = secure.getString(KEY_SERVER_URL, null) ?: BuildConfig.DEFAULT_SERVER_URL
        set(value) = secure.edit().putString(KEY_SERVER_URL, value).apply()

    var accessToken: String?
        get() = secure.getString(KEY_ACCESS, null)
        set(value) = secure.edit().putString(KEY_ACCESS, value).apply()

    var refreshToken: String?
        get() = secure.getString(KEY_REFRESH, null)
        set(value) = secure.edit().putString(KEY_REFRESH, value).apply()

    /** Стабильный идентификатор устройства: по нему сервер помечает
     *  доверенные устройства (2FA) и показывает сессии. */
    val deviceId: String
        get() = plain.getString(KEY_DEVICE_ID, null)
            ?: UUID.randomUUID().toString().also {
                plain.edit().putString(KEY_DEVICE_ID, it).apply()
            }

    val deviceName: String = "Android ${android.os.Build.MODEL}"

    fun clearTokens() {
        secure.edit().remove(KEY_ACCESS).remove(KEY_REFRESH).apply()
    }

    private companion object {
        const val KEY_SERVER_URL = "server_url"
        const val KEY_ACCESS = "access_token"
        const val KEY_REFRESH = "refresh_token"
        const val KEY_DEVICE_ID = "device_id"
    }
}
