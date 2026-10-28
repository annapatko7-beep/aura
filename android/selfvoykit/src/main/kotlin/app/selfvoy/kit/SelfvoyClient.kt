package app.selfvoy.kit

import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * Типизированный API клиента Selfvoy поверх WS-транспорта — зеркало
 * SelfvoyClient.swift (iOS). Методы повторяют имена WS-хендлеров сервера
 * (docs/PROTOCOL.md); ответ — payload кадра "ok", ошибки — SelfvoyError
 * с серверным code/message.
 */
class SelfvoyClient(private val transport: WebSocketTransport = WebSocketTransport()) {

    val state get() = transport.state
    val events get() = transport.events

    fun connect(serverUrl: String) = transport.connect(serverUrl)
    fun disconnect() = transport.disconnect()

    /** Универсальный запрос (payload собирается вызывающим кодом). */
    suspend fun request(type: String, payload: JsonObject = JsonObject(emptyMap())): JsonObject? =
        transport.request(type, payload)

    // ------------------------------------------------------------ авторизация

    /**
     * [ref] — реферальный код из приглашения: уходит (с ref_source "link") только
     * если это валидный код из 8 символов A–Z/0–9; неверный молча отбрасывается.
     */
    suspend fun register(email: String, password: String, displayName: String, ref: String? = null): JsonObject? =
        request("auth.register", buildJsonObject {
            put("email", email)
            put("password", password)
            put("display_name", displayName)
            ReferralCode.normalize(ref)?.let { code ->
                put("ref", code)
                put("ref_source", "link")
            }
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

    /** Код сброса пароля на email; ответ всегда «ок» (не раскрывает, есть ли аккаунт). */
    suspend fun forgotPassword(email: String): JsonObject? =
        request("auth.forgotPassword", buildJsonObject { put("email", email) })

    suspend fun resetPassword(email: String, code: String, newPassword: String): JsonObject? =
        request("auth.resetPassword", buildJsonObject {
            put("email", email)
            put("code", code)
            put("new_password", newPassword)
        })

    suspend fun logout(): JsonObject? = request("auth.logout")

    suspend fun setup2fa(): JsonObject? = request("auth.setup2fa")

    suspend fun confirm2fa(code: String): JsonObject? =
        request("auth.confirm2fa", buildJsonObject { put("code", code) })

    suspend fun status2fa(): JsonObject? = request("auth.status2fa")

    suspend fun disable2fa(password: String): JsonObject? =
        request("auth.disable2fa", buildJsonObject { put("password", password) })

    suspend fun sessionsList(): JsonObject? = request("sessions.list")

    /**
     * Профиль (profile.update): уходят только переданные (не null) поля.
     * Сервер: display_name 1–80 символов, bio ≤300, avatar_url — пусто, https://…
     * или data:image/{png,jpeg,webp};base64,… (до ~100 КБ); иначе bad_request.
     * Ответ — {user}, возвращаем его разобранным.
     */
    suspend fun profileUpdate(displayName: String? = null, bio: String? = null, avatarUrl: String? = null): SelfvoyUser {
        val payload = buildJsonObject {
            displayName?.let { put("display_name", it) }
            bio?.let { put("bio", it) }
            avatarUrl?.let { put("avatar_url", it) }
        }
        if (payload.isEmpty()) throw SelfvoyError("bad_request", "нет полей профиля для изменения")
        return SelfvoyUser.from(request("profile.update", payload).obj("user"))
    }

    // ---------------------------------------------------------- чаты и Selfvoy

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

    /** Запрос к Selfvoy. chatId=0 — без контекста чата (шорткаты, голос). */
    suspend fun agentAsk(message: String, chatId: Long = 0): JsonObject? =
        request("agent.ask", buildJsonObject {
            put("message", message)
            put("chat_id", chatId)
        })

    /**
     * A2A-переговоры (один раунд): target — email или имя собеседника, окно в часах.
     * Ответ: accepted, slot{start,end}, place{name,city,tags,rating}, counter_slots[], message.
     */
    suspend fun agentNegotiate(target: String, topic: String = "", durationMinutes: Int = 60, windowHours: Int = 96): JsonObject? =
        request("agent.negotiate", buildJsonObject {
            put("target", target)
            if (topic.isNotEmpty()) put("topic", topic)
            put("duration_minutes", durationMinutes)
            put("window_hours", windowHours)
        })

    /** Прямой запуск инструмента (например, book_table по кнопке пользователя). */
    suspend fun toolRun(tool: String, args: JsonObject): JsonObject? =
        request("tool.run", buildJsonObject {
            put("tool", tool)
            put("args", args)
        })

    /** Поиск людей по имени/email — для добавления в контакты (chat.open). */
    suspend fun usersSearch(query: String, limit: Int = 10): JsonObject? =
        request("users.search", buildJsonObject {
            put("query", query)
            put("limit", limit)
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

    /** Запись в долговременную память (kind: fact | preference | schedule | contact). */
    suspend fun memoryAdd(text: String, kind: String = "fact"): JsonObject? =
        request("memory.add", buildJsonObject {
            put("text", text)
            put("kind", kind)
        })

    suspend fun prefsGet(): JsonObject? = request("prefs.get")

    /**
     * Настройки: сервер читает поля (theme, notifications, city, ...) прямо из
     * payload — без обёртки (docs/PROTOCOL.md, метод prefs.set).
     */
    suspend fun prefsSet(preferences: JsonObject): JsonObject? = request("prefs.set", preferences)

    /** Флаги приватности переговоров из prefs.get (нет ключа — true). */
    suspend fun privacyGet(): Privacy = Privacy.fromPrefs(prefsGet())

    /**
     * Частичное обновление приватности: prefs.set с {"privacy":{...}} в корне payload,
     * только переданные (не null) флаги — сервер сливает их с текущими.
     * Возвращает итоговые флаги из ответа сервера.
     */
    suspend fun privacySet(
        shareFreeSlotsOnly: Boolean? = null,
        hideEventTitles: Boolean? = null,
        allowPlaceChoice: Boolean? = null,
    ): Privacy {
        val flags = buildJsonObject {
            shareFreeSlotsOnly?.let { put(Privacy.SHARE_FREE_SLOTS_ONLY, it) }
            hideEventTitles?.let { put(Privacy.HIDE_EVENT_TITLES, it) }
            allowPlaceChoice?.let { put(Privacy.ALLOW_PLACE_CHOICE, it) }
        }
        if (flags.isEmpty()) return privacyGet()
        return Privacy.fromPrefs(prefsSet(buildJsonObject { put("privacy", flags) }))
    }

    /** Все три флага разом (например, восстановление после ошибки). */
    suspend fun privacySet(privacy: Privacy): Privacy =
        Privacy.fromPrefs(prefsSet(buildJsonObject { put("privacy", privacy.toJson()) }))

    // ---------------------------------------------------------------- задачи

    suspend fun tasksList(status: String = ""): JsonObject? =
        request("tasks.list", buildJsonObject {
            if (status.isNotEmpty()) put("status", status)
        })

    suspend fun taskCreate(title: String, notes: String = "", remindAt: String = "", chatId: Long = 0, dueAt: String = ""): JsonObject? =
        request("tasks.create", buildJsonObject {
            put("title", title)
            if (notes.isNotEmpty()) put("notes", notes)
            if (remindAt.isNotEmpty()) put("remind_at", remindAt)
            if (dueAt.isNotEmpty()) put("due_at", dueAt)
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

    // ------------------------------------------------- страницы записи и рефералы

    suspend fun bookingPageCreate(fields: BookingPageFields): BookingPage {
        fields.validate()?.let { throw SelfvoyError("bad_request", it) }
        return BookingPage.from(request("booking.pageCreate", pageJson(fields, null)).obj("page"))
    }

    /** status — active | paused; fields == null — меняется только статус. */
    suspend fun bookingPageUpdate(pageId: Long, fields: BookingPageFields? = null, status: String? = null): BookingPage {
        fields?.validate()?.let { throw SelfvoyError("bad_request", it) }
        return BookingPage.from(request("booking.pageUpdate", pageJson(fields, status, pageId)).obj("page"))
    }

    suspend fun bookingPageList(): List<BookingPage> =
        request("booking.pageList").arr("pages").map { BookingPage.from(it as? JsonObject) }

    suspend fun bookingPageDelete(pageId: Long): JsonObject? =
        request("booking.pageDelete", buildJsonObject { put("page_id", pageId) })

    /** pageId 0 — все страницы. */
    suspend fun bookingList(limit: Int = 50, pageId: Long = 0, activeOnly: Boolean = false): List<Booking> =
        request("booking.list", buildJsonObject {
            put("limit", limit)
            if (pageId > 0) put("page_id", pageId)
            if (activeOnly) put("active_only", true)
        }).arr("bookings").map { Booking.from(it as? JsonObject) }

    /** occurrence — start одной встречи серии; null — все предстоящие. */
    suspend fun bookingOwnerCancel(bookingId: Long, occurrence: String? = null): Booking =
        Booking.from(request("booking.ownerCancel", buildJsonObject {
            put("booking_id", bookingId)
            if (occurrence != null) put("occurrence", occurrence)
        }).obj("booking"))

    /** Отметка оплаты владельцем (F4): status — paid | waived | refunded. */
    suspend fun bookingPaymentSet(bookingId: Long, status: String): Booking {
        if (status !in Payment.OWNER_STATUSES) throw SelfvoyError("bad_request", "неизвестный статус оплаты: $status")
        return Booking.from(request("booking.paymentSet", buildJsonObject {
            put("booking_id", bookingId)
            put("status", status)
        }).obj("booking"))
    }

    /**
     * Свободные окна своей страницы — те же, что видит гость (`booking.get` по slug).
     * Пустой список — окон нет или AI-сервис недоступен.
     */
    suspend fun bookingFreeSlots(slug: String): List<BookingSlot> =
        request("booking.get", buildJsonObject { put("slug", slug) }).obj("page").arr("slots").mapNotNull { element ->
            val slot = element as? JsonObject ?: return@mapNotNull null
            BookingSlot(slot.str("start"), slot.str("end"), slot.str("day"))
        }

    /** Пост «свободные окна» (F6): days 1–14, format telegram | vk | plain. */
    suspend fun bookingSlotsPost(pageId: Long, days: Int = 7, format: String = SlotsPost.FORMAT_TELEGRAM): SlotsPost {
        if (days !in 1..14) throw SelfvoyError("bad_request", "период — от 1 до 14 дней")
        if (format !in SlotsPost.FORMATS) throw SelfvoyError("bad_request", "неизвестный формат поста: $format")
        return SlotsPost.from(request("booking.slotsPost", buildJsonObject {
            put("page_id", pageId)
            put("days", days)
            put("format", format)
        }))
    }

    // ------------------------------------------------------ пакеты и абонементы (F5)

    /** pageId 0 — все страницы; status "" — любые. */
    suspend fun packageList(pageId: Long = 0, status: String = "", limit: Int = 50): List<PackagePurchase> {
        if (status.isNotEmpty() && status !in PackagePurchase.STATUSES) throw SelfvoyError("bad_request", "неизвестный статус пакета: $status")
        return request("package.list", buildJsonObject {
            if (pageId > 0) put("page_id", pageId)
            if (status.isNotEmpty()) put("status", status)
            put("limit", limit)
        }).arr("packages").map { PackagePurchase.from(it as? JsonObject) }
    }

    /** Выдать пакет вне сайта; токен ссылки в ответе — один раз. */
    suspend fun packageIssue(issue: PackageIssueRequest): PackageIssued {
        issue.validate()?.let { throw SelfvoyError("bad_request", it) }
        val reply = request("package.issue", issue.toJson())
        return PackageIssued(PackagePurchase.from(reply.obj("package")), reply.str("token"), reply.str("url"))
    }

    /** Корректировка остатка: delta −100..100, не 0. */
    suspend fun packageAdjust(packageId: Long, delta: Int): PackagePurchase {
        if (delta == 0 || delta !in -100..100) throw SelfvoyError("bad_request", "изменение — от −100 до 100, не 0")
        return PackagePurchase.from(request("package.adjust", buildJsonObject {
            put("package_id", packageId)
            put("delta", delta)
        }).obj("package"))
    }

    suspend fun packageCancel(packageId: Long): PackagePurchase =
        PackagePurchase.from(request("package.cancel", buildJsonObject { put("package_id", packageId) }).obj("package"))

    /** Отметка оплаты пакета: paid | waived | refunded. */
    suspend fun packagePaymentSet(packageId: Long, status: String): PackagePurchase {
        if (status !in Payment.OWNER_STATUSES) throw SelfvoyError("bad_request", "неизвестный статус оплаты: $status")
        return PackagePurchase.from(request("package.paymentSet", buildJsonObject {
            put("package_id", packageId)
            put("status", status)
        }).obj("package"))
    }

    // --------------------------------------------------------- рефералы и тариф

    suspend fun referralGet(): ReferralInfo = ReferralInfo.from(request("referral.get"))

    private fun pageJson(fields: BookingPageFields?, status: String?, pageId: Long? = null): JsonObject = buildJsonObject {
        if (pageId != null) put("page_id", pageId)
        fields?.toJson()?.forEach { (key, value) -> put(key, value) }
        if (status != null) put("status", status)
    }

    // -------------------------------------------------------- Telegram-бот (F2)

    suspend fun telegramStatus(): TelegramStatus = TelegramStatus.from(request("telegram.status"))

    /** Ссылка на бота (15 минут); бот не настроен → bad_request. */
    suspend fun telegramLink(): TelegramLink = TelegramLink.from(request("telegram.link"))

    suspend fun telegramUnlink(): JsonObject? = request("telegram.unlink")

    /**
     * Дублировать ли уведомления в Telegram (`prefs.notifications.telegram`).
     * [current] — текущий объект notifications из prefs.get: его поля (тихие часы,
     * отключённые виды) отправляются целиком, меняется только флаг.
     */
    suspend fun notificationsTelegramSet(enabled: Boolean, current: JsonObject?): JsonObject? =
        prefsSet(buildJsonObject { put("notifications", notificationsWithTelegram(current, enabled)) })

    // ------------------------------------------------------------- ЮKassa (F4)

    suspend fun paymentsStatus(): PaymentsStatus = PaymentsStatus.from(request("payments.status"))

    /** Подключить магазин; ключ проверяет сервер запросом к ЮKassa. Ключ нигде не сохраняется. */
    suspend fun paymentsConnect(credentials: YooKassaCredentials): PaymentsStatus {
        credentials.validate()?.let { throw SelfvoyError("bad_request", it) }
        return PaymentsStatus.from(request("payments.connect", credentials.toJson()))
    }

    suspend fun paymentsDisconnect(): PaymentsStatus = PaymentsStatus.from(request("payments.disconnect"))

    // ------------------------------------------------- календарь: ICS-подписка (F6)

    suspend fun calendarFeed(): CalendarFeed = CalendarFeed.from(request("calendar.feed"))

    /** Новая ссылка; прежняя перестаёт работать во всех календарях. */
    suspend fun calendarFeedReset(): CalendarFeed = CalendarFeed.from(request("calendar.feedReset"))

    suspend fun calendarFeedDisable(): CalendarFeed = CalendarFeed.from(request("calendar.feedDisable"))

    // ---------------------------------------------------------------- сервер

    suspend fun serverInfo(): JsonObject? = request("server.info")
}

// ---------------------------------------------------------- удобные доступы

fun JsonObject?.str(key: String): String =
    (this?.get(key) as? JsonPrimitive)?.takeIf { it !is kotlinx.serialization.json.JsonNull }?.content ?: ""

/** Число из поля; нет поля, null, строка-не-число или объект → 0 (терпимо к старому серверу). */
fun JsonObject?.long(key: String): Long {
    val value = this?.get(key) as? JsonPrimitive ?: return 0L
    if (value is kotlinx.serialization.json.JsonNull) return 0L
    return value.longOrNull ?: value.content.toDoubleOrNull()?.toLong() ?: 0L
}

fun JsonObject?.bool(key: String): Boolean = boolOr(key, false)

/** Флаг с умолчанием для отсутствующего поля (например, guest_reminders → true). */
fun JsonObject?.boolOr(key: String, default: Boolean): Boolean {
    val value = this?.get(key) as? JsonPrimitive ?: return default
    if (value is kotlinx.serialization.json.JsonNull) return default
    return value.content.toBooleanStrictOrNull() ?: default
}

/** prefs.notifications с флагом telegram: прочие поля (тихие часы, muted_kinds) сохраняются. */
fun notificationsWithTelegram(current: JsonObject?, enabled: Boolean): JsonObject =
    JsonObject((current ?: JsonObject(emptyMap())) + ("telegram" to JsonPrimitive(enabled)))

/** Флаг prefs.notifications.telegram (нет — true: дублировать). */
fun telegramNotificationsEnabled(prefs: JsonObject?): Boolean = prefs.obj("notifications").boolOr("telegram", true)

fun JsonObject?.obj(key: String): JsonObject? =
    this?.get(key)?.takeIf { it is JsonObject } as JsonObject?

fun JsonObject?.arr(key: String): List<JsonElement> =
    this?.get(key)?.takeIf { it is kotlinx.serialization.json.JsonArray }?.jsonArray ?: emptyList()

fun jsonOf(vararg pairs: Pair<String, JsonElement>): JsonObject =
    JsonObject(mapOf(*pairs))

fun str(value: String): JsonElement = JsonPrimitive(value)
