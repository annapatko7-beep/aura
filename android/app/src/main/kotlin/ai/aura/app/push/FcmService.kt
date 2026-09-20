package ai.aura.app.push

import ai.aura.app.AppStore
import ai.aura.app.Notifications
import ai.aura.app.R
import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * Приёмник FCM (этап 11). Сервер отправляет notification+data; в фоне карточку
 * рисует система (канал "aura"), здесь мы:
 *  1. обновляем токен на сервере при ротации (devices.push.register, upsert);
 *  2. в foreground показываем карточку сами и обновляем «входящую» приложения.
 *
 * Никаких фоновых процессов и listeners сверх официального FCM SDK.
 */
class FcmService : FirebaseMessagingService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onNewToken(token: String) {
        // Регистрация повторит upsert на сервере, если пользователь в сети.
        AppStore.reRegisterPushToken(token)
    }

    override fun onMessageReceived(message: RemoteMessage) {
        val title = message.notification?.title ?: message.data["kind"] ?: "Аура"
        val body = message.notification?.body ?: ""
        val kind = message.data["kind"] ?: ""
        Notifications.show(this, title, body, kind)
        // Данные могли измениться (уведомление создано другим устройством) —
        // тянем свежий список, если сессия жива.
        scope.launch { runCatching { AppStore.refreshNotificationsBlocking() } }
    }

    companion object {
        const val CHANNEL_ID: String = Notifications.CHANNEL_ID
        val SMALL_ICON: Int = R.drawable.ic_stat_aura
    }
}
