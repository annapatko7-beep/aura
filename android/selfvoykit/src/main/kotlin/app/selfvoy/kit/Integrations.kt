package app.selfvoy.kit

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/*
 * Подключения владельца: Telegram-бот (F2), ЮKassa (F4) и ICS-подписка на
 * календарь (F6) — docs/PROTOCOL.md → «Telegram-бот», «Цена, предоплата и
 * политика отмены», «Календарь: ICS-подписка».
 */

/** `telegram.status`: бот настроен на сервере (`enabled`), привязан ли чат владельца. */
data class TelegramStatus(val enabled: Boolean, val botUsername: String, val linked: Boolean, val username: String) {
    companion object {
        fun from(json: JsonObject?): TelegramStatus =
            TelegramStatus(json.bool("enabled"), json.str("bot_username"), json.bool("linked"), json.str("username"))
    }
}

/** `telegram.link`: ссылка на бота, живёт 15 минут. */
data class TelegramLink(val url: String, val expiresAt: String) {
    companion object {
        fun from(json: JsonObject?): TelegramLink = TelegramLink(json.str("url"), json.str("expires_at"))
    }
}

/** `payments.status` / `payments.connect`: подключён ли магазин ЮKassa. Секрет сервер не отдаёт. */
data class PaymentsStatus(val connected: Boolean, val provider: String, val shopId: String, val connectedAt: String) {
    companion object {
        const val YOOKASSA = "yookassa"

        fun from(json: JsonObject?): PaymentsStatus =
            PaymentsStatus(json.bool("connected"), json.str("provider"), json.str("shop_id"), json.str("connected_at"))
    }
}

/**
 * Данные подключения ЮKassa. Секретный ключ живёт только здесь до отправки:
 * не сохраняется и не попадает в toString (журналы, отчёты о сбоях).
 */
class YooKassaCredentials(shopId: String, secretKey: String) {
    val shopId: String = shopId.trim()
    private val secret: String = secretKey.trim()

    /** Ошибка для пользователя или null — те же проверки формы, что у сервера. */
    fun validate(): String? = when {
        !SHOP_ID.matches(shopId) -> "shopId — только цифры, до 20 знаков"
        !SECRET.matches(secret) -> "Секретный ключ начинается с live_ или test_"
        else -> null
    }

    internal fun toJson(): JsonObject = buildJsonObject {
        put("provider", PaymentsStatus.YOOKASSA)
        put("shop_id", shopId)
        put("secret_key", secret)
    }

    override fun toString(): String = "YooKassaCredentials(shopId=$shopId, secretKey=***)"

    companion object {
        private val SHOP_ID = Regex("^\\d{1,20}$")
        private val SECRET = Regex("^(live|test)_\\S+$")
    }
}

/**
 * ICS-подписка (`calendar.feed*`). `url`/`path` непусты только в ответе
 * `calendar.feedReset` — токен сервер не хранит и второй раз не покажет.
 */
data class CalendarFeed(val enabled: Boolean, val url: String, val path: String) {

    /**
     * Адрес подписки: готовый `url` сервера или собранный из адреса WS-сервера
     * (wss → https, ws → http, тот же хост и порт) плюс `path`. Нечего собрать → null.
     */
    fun feedUrl(serverUrl: String): String? {
        val ready = url.trim()
        if (ready.startsWith("https://", ignoreCase = true) || ready.startsWith("http://", ignoreCase = true)) return ready
        val tail = path.trim()
        if (!tail.startsWith("/")) return null
        val server = serverUrl.trim()
        val scheme = when {
            server.startsWith("wss://", ignoreCase = true) -> "https://"
            server.startsWith("ws://", ignoreCase = true) -> "http://"
            else -> return null
        }
        val host = server.substringAfter("://").substringBefore('/').substringBefore('?').substringBefore('#')
        if (host.isEmpty()) return null
        return scheme + host + tail
    }

    companion object {
        fun from(json: JsonObject?): CalendarFeed = CalendarFeed(json.bool("enabled"), json.str("url"), json.str("path"))

        /** webcal:// — календари на телефоне открывают подписку сразу; только для https. */
        fun webcal(url: String?): String? =
            if (url != null && url.startsWith("https://", ignoreCase = true)) "webcal://" + url.substring("https://".length) else null
    }
}
