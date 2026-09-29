package ai.aura.app

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat

/**
 * Канал и показ локальных уведомлений. CHANNEL_ID совпадает с channel_id,
 * который сервер кладёт в android-конфиг FCM-сообщения (notificationsmanager),
 * поэтому системная и локальная доставка попадают в один канал.
 */
object Notifications {

    const val CHANNEL_ID = "aura"
    private var counter = 1000

    fun ensureChannel(context: Context) {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "События Ауры",
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = "Напоминания, подтверждения операций и предложения других агентов"
        }
        context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
    }

    fun show(context: Context, title: String, body: String, kind: String) {
        ensureChannel(context)
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)
            != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return  // разрешения нет — молча не спамим; UI запрашивает его явно
        }
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
            data = android.net.Uri.parse("aura://notifications")
        }
        val pending = PendingIntent.getActivity(
            context, counter, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_aura)
            .setContentTitle(title)
            .setContentText(body)
            .setStyle(NotificationCompat.BigTextStyle().bigText(body))
            .setContentIntent(pending)
            .setAutoCancel(true)
            .build()
        NotificationManagerCompat.from(context).notify(counter++, notification)
    }
}
