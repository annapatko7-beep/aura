package ai.aura.kit

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put

/**
 * Типизированный API клиента Aura поверх WS-транспорта — зеркало
 * AuraClient.swift (iOS). Методы повторяют имена WS-хендлеров сервера
 * (docs/PROTOCOL.md); ответ — payload кадра "ok", ошибки — AuraError
 * с серверным code/message.
 */
class AuraClient(private val transport: WebSocketTransport = WebSocketTransport()) {

    val state get() = transport.state
    val events get() = transport.events

    fun connect(serverUrl: String) = transport.connect(serverUrl)
    fun disconnect() = transport.disconnect()

    /** Универсальный запрос (payload собирается вызывающим кодом). */
    suspend fun request(type: String, payload: JsonObject = JsonObject(emptyMap())): JsonObject? =
        transport.request(type, payload)

    // ------------------------------------------------------------ авторизация

    suspend fun register(email: String, password: String, displayName: String): JsonObject? =
        request("auth.register", buildJsonObject {
            put("email", email)
            put("password", password)
            put("display_name", displayName)
        })

    suspend fun verifyEmail(email: String, code: String): JsonObject? =
        request("auth.verifyEmail", buildJsonObject {
            put("email", email)
            put("code", code)
        })

    suspend fun resendCode(email: String): JsonObject? =
        request("auth.resendCode", buildJsonObject { put("email", email) })

    suspend fun login(email: String, password: String, deviceId: String, device: String): JsonObject? =
        request("auth.login", buildJsonObject {
            put("email", email)
            put("password", password)
            put("device_id", deviceId)
            put("device", device)
        })

    /** Завершение входа с TOTP-кодом или резервным кодом (2FA). */
    suspend fun login2fa(email: String, password: String, code: String, trustDevice: Boolean, deviceId: String, device: String): JsonObject? =
        request("auth.login2fa", buildJsonObject {
            put("email", email)
            put("password", password)
            put("code", code)
            put("trust_device", trustDevice)
            put("device_id", deviceId)
            put("device", device)
        })

    /** Повторная аутентификация соединения сохранённым access-токеном. */
    suspend fun authenticate(token: String): JsonObject? =
        request("auth.token", buildJsonObject { put("token", token) })

    suspend fun refresh(refreshToken: String, device: String): JsonObject? =
        request("auth.refresh", buildJsonObject {
            put("refresh_token", refreshToken)
            put("device", device)
        })

    suspend fun me(): JsonObject? = request("auth.me")

    suspend fun logout(): JsonObject? = request("auth.logout")

    suspend fun setup2fa(): JsonObject? = request("auth.setup2fa")

    suspend fun confirm2fa(code: String): JsonObject? =
        request("auth.confirm2fa", buildJsonObject { put("code", code) })

    suspend fun status2fa(): JsonObject? = request("auth.status2fa")

    suspend fun disable2fa(password: String): JsonObject? =
        request("auth.disable2fa", buildJsonObject { put("password", password) })

    suspend fun sessionsList(): JsonObject? = request("sessions.list")

    // ---------------------------------------------------------- чаты и Аура

    suspend fun chatList(limit: Int = 50): JsonObject? =
        request("chat.list", buildJsonObject { put("limit", limit) })

    suspend fun chatOpen(contact: String): JsonObject? =
        request("chat.open", buildJsonObject { put("contact", contact) })

    suspend fun chatHistory(chatId: Long, limit: Int = 50): JsonObject? =
        request("chat.history", buildJsonObject {
            put("chat_id", chatId)
            put("limit", limit)
        })

    /** Поле протокола — "body" (docs/PROTOCOL.md); сервер читает именно его. */
    suspend fun chatSend(chatId: Long, text: String): JsonObject? =
        request("chat.send", buildJsonObject {
            put("chat_id", chatId)
            put("body", text)
        })

    /** Запрос к Ауре. chatId=0 — без контекста чата (шорткаты, голос). */
    suspend fun agentAsk(message: String, chatId: Long = 0): JsonObject? =
        request("agent.ask", buildJsonObject {
            put("message", message)
            put("chat_id", chatId)
        })

    /** Серверный STT: аудио в base64 (16 кГц моно WAV), fallback для on-device. */
    suspend fun speechTranscribe(audioBase64: String, language: String = "auto", format: String = "wav"): JsonObject? =
        request("speech.transcribe", buildJsonObject {
            put("audio", audioBase64)
            put("language", language)
            put("format", format)
        })

    // -------------------------------------------------------- память и_prefs

    suspend fun memoryList(query: String = ""): JsonObject? =
        request("memory.list", buildJsonObject {
            if (query.isNotEmpty()) put("query", query)
        })

    suspend fun prefsGet(): JsonObject? = request("prefs.get")

    /**
     * Настройки: сервер читает поля (theme, notifications, city, ...) прямо из
     * payload — без обёртки (docs/PROTOCOL.md, метод prefs.set).
     */
    suspend fun prefsSet(preferences: JsonObject): JsonObject? = request("prefs.set", preferences)

    // ---------------------------------------------------------------- задачи

    suspend fun tasksList(status: String = ""): JsonObject? =
        request("tasks.list", buildJsonObject {
            if (status.isNotEmpty()) put("status", status)
        })

    suspend fun taskCreate(title: String, notes: String = "", remindAt: String = "", chatId: Long = 0): JsonObject? =
        request("tasks.create", buildJsonObject {
            put("title", title)
            if (notes.isNotEmpty()) put("notes", notes)
            if (remindAt.isNotEmpty()) put("remind_at", remindAt)
            if (chatId > 0) put("chat_id", chatId)
        })

    /** status: complete | cancel | reopen (тип собирается как tasks.<status>). */
    suspend fun taskSetStatus(id: Long, status: String): JsonObject? =
        request("tasks.$status", buildJsonObject { put("id", id) })

    suspend fun taskDelete(id: Long): JsonObject? =
        request("tasks.delete", buildJsonObject { put("id", id) })

    // ------------------------------------------- разрешения и подтверждения

    suspend fun permissionsList(): JsonObject? = request("permissions.list")

    suspend fun permissionSet(tool: String, mode: String): JsonObject? =
        request("permissions.set", buildJsonObject {
            put("tool", tool)
            put("mode", mode)
        })

    suspend fun confirmationsList(status: String = "pending"): JsonObject? =
        request("confirmation.list", buildJsonObject { put("status", status) })

    suspend fun confirmationDecide(id: Long, approve: Boolean): JsonObject? =
        request(if (approve) "confirmation.approve" else "confirmation.deny",
            buildJsonObject { put("id", id) })

    // ----------------------------------------------------------- интеграции

    suspend fun integrationsList(): JsonObject? = request("integrations.list")

    suspend fun integrationBegin(provider: String, redirectUri: String): JsonObject? =
        request("integrations.begin", buildJsonObject {
            put("provider", provider)
            put("redirect_uri", redirectUri)
        })

    suspend fun integrationCallback(provider: String, code: String, state: String): JsonObject? =
        request("integrations.callback", buildJsonObject {
            put("provider", provider)
            put("code", code)
            put("state", state)
        })

    suspend fun integrationRevoke(id: Long): JsonObject? =
        request("integrations.revoke", buildJsonObject { put("id", id) })

    suspend fun integrationSync(id: Long): JsonObject? =
        request("integrations.sync", buildJsonObject { put("id", id) })

    // --------------------------------------- уведомления и push (этапы 11/13)

    suspend fun notificationsList(unreadOnly: Boolean = false, limit: Int = 50): JsonObject? =
        request("notifications.list", buildJsonObject {
            put("unread", unreadOnly)
            put("limit", limit)
        })

    /** id = 0 — отметить прочитанными все. */
    suspend fun notificationsRead(id: Long = 0): JsonObject? =
        request("notifications.read", buildJsonObject { put("id", id) })

    /** platform для Android — "fcm" (токен Firebase Cloud Messaging). */
    suspend fun devicesPushRegister(platform: String, token: String): JsonObject? =
        request("devices.push.register", buildJsonObject {
            put("platform", platform)
            put("token", token)
        })

    suspend fun devicesPushList(): JsonObject? = request("devices.push.list")

    suspend fun devicesPushRevoke(id: Long): JsonObject? =
        request("devices.push.revoke", buildJsonObject { put("id", id) })

    // ---------------------------------------------------------------- сервер

    suspend fun serverInfo(): JsonObject? = request("server.info")
}

// ---------------------------------------------------------- удобные доступы

fun JsonObject?.str(key: String): String =
    this?.get(key)?.jsonPrimitive?.takeIf { it !is kotlinx.serialization.json.JsonNull }?.content ?: ""

fun JsonObject?.long(key: String): Long =
    this?.get(key)?.jsonPrimitive?.long ?: 0L

fun JsonObject?.bool(key: String): Boolean =
    this?.get(key)?.jsonPrimitive?.content?.toBooleanStrictOrNull() ?: false

fun JsonObject?.obj(key: String): JsonObject? =
    this?.get(key)?.takeIf { it is JsonObject } as JsonObject?

fun JsonObject?.arr(key: String): List<JsonElement> =
    this?.get(key)?.takeIf { it is kotlinx.serialization.json.JsonArray }?.jsonArray ?: emptyList()

fun jsonOf(vararg pairs: Pair<String, JsonElement>): JsonObject =
    JsonObject(mapOf(*pairs))

fun str(value: String): JsonElement = JsonPrimitive(value)
