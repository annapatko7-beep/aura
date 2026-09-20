package ai.aura.kit

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject

// Кадры WS-протокола Aura — те же, что у C++/Qt/Swift-клиентов
// (docs/PROTOCOL.md):
//
//   запрос клиента:  {"id": "...", "type": "auth.login", "payload": {...}}
//   ответ сервера:   {"id": "...", "type": "ok",    "payload": {...}}
//                    {"id": "...", "type": "error", "code": "...", "message": "..."}
//   событие сервера: {"type": "event", "event": "chat.message", "payload": {...}}

@Serializable
data class AuraRequestFrame(
    val id: String,
    val type: String,
    val payload: JsonObject,
)

@Serializable
data class AuraResponseFrame(
    val id: String? = null,
    val type: String? = null,
    val code: String? = null,
    val message: String? = null,
    val event: String? = null,
    val payload: JsonObject? = null,
)

/** Ошибка протокола: сервер вернул кадр {"type":"error", code, message}. */
data class AuraError(val code: String, val message: String) :
    Exception("$code: $message")

/** Имена событий сервера (push внутри WS-сессии). */
object AuraEventName {
    const val CHAT_MESSAGE = "chat.message"
    const val TASK_DUE = "task.due"
    const val NOTIFICATION_NEW = "notification.new"
}
