package app.selfvoy.kit

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject

// Кадры WS-протокола Selfvoy — те же, что у C++/Qt/Swift-клиентов
// (docs/PROTOCOL.md):
//
//   запрос клиента:  {"id": "...", "type": "auth.login", "payload": {...}}
//   ответ сервера:   {"id": "...", "type": "ok",    "payload": {...}}
//                    {"id": "...", "type": "error", "code": "...", "message": "..."}
//   событие сервера: {"type": "event", "event": "chat.message", "payload": {...}}

@Serializable
data class SelfvoyRequestFrame(
    val id: String,
    val type: String,
    val payload: JsonObject,
)

@Serializable
data class SelfvoyResponseFrame(
    val id: String? = null,
    val type: String? = null,
    val code: String? = null,
    val message: String? = null,
    val event: String? = null,
    val payload: JsonObject? = null,
)

/**
 * Ошибка протокола: сервер вернул кадр {"type":"error", code, message}.
 * Поле называется serverMessage, т.к. `message` в Exception занято;
 * текст ошибки доступен через унаследованный `message` ("code: message").
 */
data class SelfvoyError(val code: String, val serverMessage: String) :
    Exception("$code: $serverMessage") {
    /** Действие доступно только на тарифе Pro (`pro_required`, docs/PROTOCOL.md → «Коды ошибок»). */
    val isProRequired: Boolean get() = code == SelfvoyErrorCode.PRO_REQUIRED
}

/** Коды ошибок сервера, на которые клиент реагирует особо. */
object SelfvoyErrorCode {
    const val BAD_REQUEST = "bad_request"
    const val CONFLICT = "conflict"
    const val PRO_REQUIRED = "pro_required"

    /** Что показать на `pro_required`: зачем Pro и как его получить бесплатно. */
    const val PRO_REQUIRED_TEXT =
        "Это доступно на тарифе Pro. Пригласите коллегу в Selfvoy — вы оба получите месяц Pro (Настройки → Пригласить друзей)."
}

/** Имена событий сервера (push внутри WS-сессии). */
object SelfvoyEventName {
    const val CHAT_MESSAGE = "chat.message"
    const val TASK_DUE = "task.due"
    const val NOTIFICATION_NEW = "notification.new"
    /** Сессию соединения отозвали (выход/смена пароля на другом устройстве, блокировка). */
    const val SESSION_REVOKED = "session.revoked"
}

/**
 * Виды уведомлений (`notification.new` → `kind`), на которые реагирует владелец:
 * записи, оплаты, пакеты, рефералы (docs/PROTOCOL.md → «Уведомления и push»).
 */
object SelfvoyNoticeKind {
    const val BOOKING_CREATED = "booking.created"
    const val BOOKING_RESCHEDULED = "booking.rescheduled"
    const val BOOKING_CANCELLED = "booking.cancelled"
    const val BOOKING_CONFIRMED = "booking.confirmed"
    const val BOOKING_PAYMENT = "booking.payment"
    const val PACKAGE_PURCHASED = "package.purchased"
    const val PACKAGE_PAYMENT = "package.payment"
    const val PACKAGE_LOW = "package.low"
    const val REFERRAL_JOINED = "referral.joined"
    const val REFERRAL_REWARD = "referral.reward"

    /** Уведомление меняет записи владельца — список записей стоит перечитать. */
    fun touchesBookings(kind: String): Boolean = kind.startsWith("booking.") || kind == PACKAGE_PURCHASED

    /** Уведомление меняет абонементы. */
    fun touchesPackages(kind: String): Boolean = kind.startsWith("package.")

    /** Уведомление меняет тариф или счётчики приглашений. */
    fun touchesReferral(kind: String): Boolean = kind == REFERRAL_JOINED || kind == REFERRAL_REWARD
}
