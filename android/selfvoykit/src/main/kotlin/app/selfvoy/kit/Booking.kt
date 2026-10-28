package app.selfvoy.kit

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/*
 * Страницы записи (booking.*), оплата (F4), пакеты и абонементы (F5) и рефералы
 * (referral.*) — модели ответа сервера (docs/PROTOCOL.md → «Страницы записи»,
 * «Цена, предоплата и политика отмены», «Пакеты, абонементы и второй получатель»,
 * «Рефералы»). Разбор терпим к отсутствующим полям: старый сервер не роняет клиент.
 */

/** Предложение пакета на странице (`packages[]`): «10 занятий за 18 000 ₽ на 90 дней». */
data class PackageOffer(
    /** 0 — новое предложение (id выдаст сервер). */
    val id: Long = 0,
    val title: String,
    val sessions: Int,
    /** Цена в минимальных единицах валюты страницы. */
    val price: Long,
    val validDays: Int = 90,
) {
    fun validate(): String? {
        val name = title.trim()
        return when {
            name.isEmpty() -> "Назовите пакет — например, «10 занятий»"
            name.codePointCount(0, name.length) > TITLE_MAX || name.contains('\n') -> "Название пакета — до $TITLE_MAX символов в одну строку"
            sessions !in 2..100 -> "В пакете — от 2 до 100 занятий"
            price <= 0 || price > Money.PRICE_MAX -> "Укажите цену пакета"
            validDays !in 7..365 -> "Срок пакета — от 7 до 365 дней"
            else -> null
        }
    }

    fun toJson(): JsonObject = buildJsonObject {
        if (id > 0) put("id", id)
        put("title", title.trim())
        put("sessions", sessions)
        put("price", price)
        put("valid_days", validDays)
    }

    companion object {
        const val TITLE_MAX = 80
        const val MAX_OFFERS = 5

        fun from(json: JsonObject?): PackageOffer = PackageOffer(
            id = json.long("id"),
            title = json.str("title"),
            sessions = json.long("sessions").toInt(),
            price = json.long("price"),
            validDays = json.long("valid_days").toInt().takeIf { it > 0 } ?: 90,
        )
    }
}

/** Значения полей оплаты и отмены — как их принимает сервер. */
object BookingPolicy {
    const val PREPAY_NONE = "none"
    const val PREPAY_FULL = "full"
    const val PREPAY_DEPOSIT = "deposit"
    val PREPAY = listOf(PREPAY_NONE, PREPAY_FULL, PREPAY_DEPOSIT)

    const val LATE_ALLOW = "allow"
    const val LATE_BLOCK = "block"
    val LATE = listOf(LATE_ALLOW, LATE_BLOCK)

    const val PAYMENT_URL_MAX = 500
    const val PAYMENT_NOTE_MAX = 300
    const val POLICY_MAX = 500
}

/** Поля страницы для booking.pageCreate / pageUpdate; пределы — как на сервере. */
data class BookingPageFields(
    val title: String,
    val description: String = "",
    val durationMinutes: Int = 30,
    val daysAhead: Int = 14,
    val maxPerDay: Int = 4,
    val noticeHours: Int = 2,
    val maxSeries: Int = 1,
    val weekends: Boolean = false,
    /** F1: напоминать гостю за сутки и за 2 часа (бесплатно). */
    val guestReminders: Boolean = true,
    /** F3: скрыть подпись «Запись через Selfvoy» (Pro). */
    val hideBranding: Boolean = false,
    /** F4: цена в минимальных единицах; 0 — не показывать. */
    val price: Long = 0,
    val currency: String = "RUB",
    val prepay: String = BookingPolicy.PREPAY_NONE,
    val deposit: Long = 0,
    val paymentUrl: String = "",
    val paymentNote: String = "",
    val payWithinMinutes: Int = 60,
    val cancelHours: Int = 0,
    val lateCancel: String = BookingPolicy.LATE_ALLOW,
    val policyText: String = "",
    /** F5: предложения пакетов (Pro), не больше 5. */
    val packages: List<PackageOffer> = emptyList(),
) {
    /** Ошибка для пользователя или null — те же пределы, что проверяет сервер. */
    fun validate(): String? {
        val name = title.trim()
        val url = paymentUrl.trim()
        val note = paymentNote.trim()
        val policy = policyText.trim()
        return when {
            name.isEmpty() -> "Назовите страницу — например, «Консультация»"
            name.codePointCount(0, name.length) > TITLE_MAX || name.contains('\n') ->
                "Название — до $TITLE_MAX символов в одну строку"
            description.trim().let { it.codePointCount(0, it.length) } > DESCRIPTION_MAX ->
                "Описание — до $DESCRIPTION_MAX символов"
            durationMinutes !in 15..240 -> "Длительность — от 15 до 240 минут"
            daysAhead !in 1..60 -> "Запись вперёд — от 1 до 60 дней"
            maxPerDay !in 1..16 -> "Встреч в день — от 1 до 16"
            noticeHours !in 0..168 -> "Запас до встречи — от 0 до 168 часов"
            maxSeries !in 1..12 -> "Серия — от 1 до 12 встреч"
            price !in 0..Money.PRICE_MAX -> "Цена — не больше 1 000 000"
            Money.CURRENCIES.none { it.code == currency } -> "Неизвестная валюта"
            prepay !in BookingPolicy.PREPAY -> "Неизвестный вид предоплаты"
            prepay != BookingPolicy.PREPAY_NONE && price <= 0 -> "Для предоплаты укажите цену"
            prepay == BookingPolicy.PREPAY_DEPOSIT && (deposit < 1 || deposit > price) -> "Депозит — больше нуля и не больше цены"
            url.isNotEmpty() && (!url.startsWith("https://") || url.length > BookingPolicy.PAYMENT_URL_MAX || url.any { it.isWhitespace() }) ->
                "Ссылка оплаты — https://…, без пробелов, до ${BookingPolicy.PAYMENT_URL_MAX} символов"
            note.codePointCount(0, note.length) > BookingPolicy.PAYMENT_NOTE_MAX || note.contains('\n') ->
                "Реквизиты — одна строка до ${BookingPolicy.PAYMENT_NOTE_MAX} символов"
            payWithinMinutes !in 15..1440 -> "Срок оплаты — от 15 до 1440 минут"
            cancelHours !in 0..168 -> "Бесплатная отмена — от 0 до 168 часов"
            lateCancel !in BookingPolicy.LATE -> "Неизвестное правило поздней отмены"
            policy.codePointCount(0, policy.length) > BookingPolicy.POLICY_MAX -> "Правила отмены — до ${BookingPolicy.POLICY_MAX} символов"
            packages.size > PackageOffer.MAX_OFFERS -> "Пакетов — не больше ${PackageOffer.MAX_OFFERS}"
            else -> packages.firstNotNullOfOrNull { it.validate() }
        }
    }

    /** Все поля страницы для pageCreate/pageUpdate (packages — целым массивом). */
    fun toJson(): JsonObject = buildJsonObject {
        put("title", title.trim())
        put("description", description.trim())
        put("duration_minutes", durationMinutes)
        put("days_ahead", daysAhead)
        put("max_per_day", maxPerDay)
        put("notice_hours", noticeHours)
        put("max_series", maxSeries)
        put("weekends", weekends)
        put("guest_reminders", guestReminders)
        put("hide_branding", hideBranding)
        put("price", price)
        put("currency", currency)
        put("prepay", prepay)
        put("deposit", if (prepay == BookingPolicy.PREPAY_DEPOSIT) deposit else 0L)
        put("payment_url", paymentUrl.trim())
        put("payment_note", paymentNote.trim())
        put("pay_within_minutes", payWithinMinutes)
        put("cancel_hours", cancelHours)
        put("late_cancel", lateCancel)
        put("policy_text", policyText.trim())
        put("packages", JsonArray(packages.map { it.toJson() }))
    }

    companion object {
        const val TITLE_MAX = 120
        const val DESCRIPTION_MAX = 1000

        /** Ключи payload, которые отправляет [toJson] (для тестов и проверок). */
        val KEYS = setOf(
            "title", "description", "duration_minutes", "days_ahead", "max_per_day", "notice_hours",
            "max_series", "weekends", "guest_reminders", "hide_branding", "price", "currency", "prepay",
            "deposit", "payment_url", "payment_note", "pay_within_minutes", "cancel_hours", "late_cancel",
            "policy_text", "packages",
        )
    }
}

data class BookingPage(
    val id: Long,
    val slug: String,
    /** Готовая ссылка или "" — если на сервере не задан SELFVOY_PUBLIC_URL. */
    val url: String,
    val title: String,
    val description: String,
    val durationMinutes: Int,
    val daysAhead: Int,
    val maxPerDay: Int,
    val noticeHours: Int,
    val maxSeries: Int,
    val weekends: Boolean,
    /** active | paused */
    val status: String,
    val upcoming: Int,
    val guestReminders: Boolean = true,
    val hideBranding: Boolean = false,
    val price: Long = 0,
    val currency: String = "RUB",
    val prepay: String = BookingPolicy.PREPAY_NONE,
    val deposit: Long = 0,
    val paymentUrl: String = "",
    val paymentNote: String = "",
    val payWithinMinutes: Int = 60,
    val cancelHours: Int = 0,
    val lateCancel: String = BookingPolicy.LATE_ALLOW,
    val policyText: String = "",
    val packages: List<PackageOffer> = emptyList(),
) {
    val fields get() = BookingPageFields(
        title, description, durationMinutes, daysAhead, maxPerDay, noticeHours, maxSeries, weekends,
        guestReminders, hideBranding, price, currency, prepay, deposit, paymentUrl, paymentNote,
        payWithinMinutes, cancelHours, lateCancel, policyText, packages,
    )

    /** Ссылка для гостя: от сервера или путь веб-клиента (собрать полный адрес нельзя). */
    val link: String get() = url.ifEmpty { "#/b/$slug" }

    val active: Boolean get() = status == "active"

    companion object {
        fun from(json: JsonObject?): BookingPage = BookingPage(
            id = json.long("id"),
            slug = json.str("slug"),
            url = json.str("url"),
            title = json.str("title"),
            description = json.str("description"),
            durationMinutes = json.long("duration_minutes").toInt(),
            daysAhead = json.long("days_ahead").toInt(),
            maxPerDay = json.long("max_per_day").toInt(),
            noticeHours = json.long("notice_hours").toInt(),
            maxSeries = json.long("max_series").toInt(),
            weekends = json.bool("weekends"),
            status = json.str("status"),
            upcoming = json.long("upcoming").toInt(),
            guestReminders = json.boolOr("guest_reminders", true),
            hideBranding = json.bool("hide_branding"),
            price = json.long("price"),
            currency = json.str("currency").ifEmpty { "RUB" },
            prepay = json.str("prepay").ifEmpty { BookingPolicy.PREPAY_NONE },
            deposit = json.long("deposit"),
            paymentUrl = json.str("payment_url"),
            paymentNote = json.str("payment_note"),
            payWithinMinutes = json.long("pay_within_minutes").toInt().takeIf { it > 0 } ?: 60,
            cancelHours = json.long("cancel_hours").toInt(),
            lateCancel = json.str("late_cancel").ifEmpty { BookingPolicy.LATE_ALLOW },
            policyText = json.str("policy_text"),
            packages = json.arr("packages").mapNotNull { (it as? JsonObject)?.let(PackageOffer::from) },
        )
    }
}

/**
 * Оплата записи или пакета (`payment`, F4): статус pending → paid / claimed / expired;
 * владелец ставит paid, waived или refunded (`booking.paymentSet`, `package.paymentSet`).
 */
data class Payment(
    val status: String,
    val amount: Long,
    val currency: String,
    /** full | deposit | package */
    val kind: String = "",
    /** link | yookassa */
    val method: String = "",
    val actionUrl: String = "",
    val dueAt: String = "",
    val paidAt: String = "",
    val refundedAt: String = "",
) {
    /** Ждёт денег от гостя (для ленты дня и карточки). */
    val awaiting: Boolean get() = status == PENDING || status == CLAIMED

    /** Что может отметить владелец: ждём → «оплачено» / «без оплаты»; оплачено → «возврат». */
    val ownerActions: List<String> get() = when (status) {
        PENDING, CLAIMED -> listOf(PAID, WAIVED)
        PAID -> listOf(REFUNDED)
        WAIVED, EXPIRED -> listOf(PAID)
        else -> emptyList()
    }

    companion object {
        const val PENDING = "pending"
        const val CLAIMED = "claimed"
        const val PAID = "paid"
        const val WAIVED = "waived"
        const val REFUNDED = "refunded"
        const val EXPIRED = "expired"

        /** Статусы, которые может поставить владелец. */
        val OWNER_STATUSES = setOf(PAID, WAIVED, REFUNDED)

        /** null — оплаты нет (поле отсутствует или null). */
        fun from(json: JsonObject?): Payment? {
            if (json == null || json.str("status").isEmpty()) return null
            return Payment(
                status = json.str("status"),
                amount = json.long("amount"),
                currency = json.str("currency").ifEmpty { "RUB" },
                kind = json.str("kind"),
                method = json.str("method"),
                actionUrl = json.str("action_url"),
                dueAt = json.str("due_at"),
                paidAt = json.str("paid_at"),
                refundedAt = json.str("refunded_at"),
            )
        }
    }
}

data class BookingOccurrence(
    val start: String,
    val end: String,
    val status: String,
    val local: String = "",
    /** F1: когда гость нажал «Приду»; "" — не подтверждал. */
    val confirmedAt: String = "",
) {
    val active get() = status == "active"
    val confirmed get() = confirmedAt.isNotEmpty()
}

/** Напоминание гостю (F1): kind 24h | 2h, sent_at "" — ещё не отправлено. */
data class BookingReminder(val start: String, val kind: String, val dueAt: String, val sentAt: String) {
    val sent get() = sentAt.isNotEmpty()
}

/** Запись гостя глазами владельца. */
data class Booking(
    val id: Long,
    val pageId: Long,
    val pageTitle: String,
    val guestName: String,
    val guestEmail: String,
    val guestComment: String,
    /** active | cancelled */
    val status: String,
    /** "" | guest | owner | system (оплата не поступила) */
    val cancelledBy: String,
    val reschedules: Int,
    val occurrences: List<BookingOccurrence>,
    /** F5: второй получатель извещений ("" — нет). */
    val ccEmail: String = "",
    /** F5: пакет записи (0 — без пакета). */
    val packageId: Long = 0,
    val reminders: List<BookingReminder> = emptyList(),
    val payment: Payment? = null,
    val createdAt: String = "",
    val updatedAt: String = "",
    /**
     * Цена услуги, зафиксированная при записи (минимальные единицы): 0 — занятие по
     * абонементу или бесплатная услуга; null — старый сервер без поля (берём цену страницы).
     * Не путать с [Payment.amount] — это сумма к оплате онлайн (депозит или предоплата).
     */
    val price: Long? = null,
    /** Валюта [price]; "" — старый сервер. */
    val currency: String = "",
) {
    val active get() = status == "active"

    /** Занятие по абонементу: деньги взяты при покупке пакета. */
    val byPackage get() = packageId > 0

    /** Стоимость встречи: цена записи; старый сервер — цена страницы; по абонементу — 0. */
    fun priceWith(page: BookingPage?): Long = when {
        byPackage -> 0L
        price != null -> price.coerceAtLeast(0L)
        else -> page?.price?.coerceAtLeast(0L) ?: 0L
    }

    /** Валюта стоимости встречи (запись → страница → RUB). */
    fun currencyWith(page: BookingPage?): String =
        currency.ifEmpty { page?.currency.orEmpty() }.ifEmpty { "RUB" }

    companion object {
        fun from(json: JsonObject?): Booking = Booking(
            id = json.long("id"),
            pageId = json.long("page_id"),
            pageTitle = json.str("page_title"),
            guestName = json.str("guest_name"),
            guestEmail = json.str("guest_email"),
            guestComment = json.str("guest_comment"),
            status = json.str("status"),
            cancelledBy = json.str("cancelled_by"),
            reschedules = json.long("reschedules").toInt(),
            occurrences = json.arr("occurrences").mapNotNull { element ->
                val item = element as? JsonObject ?: return@mapNotNull null
                BookingOccurrence(item.str("start"), item.str("end"), item.str("status"), item.str("local"),
                    item.str("confirmed_at"))
            }.sortedBy { it.start },
            ccEmail = json.str("cc_email"),
            packageId = json.long("package_id"),
            reminders = json.arr("reminders").mapNotNull { element ->
                val item = element as? JsonObject ?: return@mapNotNull null
                BookingReminder(item.str("start"), item.str("kind"), item.str("due_at"), item.str("sent_at"))
            },
            payment = Payment.from(json.obj("payment")),
            createdAt = json.str("created_at"),
            updatedAt = json.str("updated_at"),
            price = if (json.present("price")) json.long("price") else null,
            currency = json.str("currency"),
        )

        private fun JsonObject?.present(key: String): Boolean {
            val value = this?.get(key) ?: return false
            return value !is kotlinx.serialization.json.JsonNull
        }
    }
}

/**
 * Деньги записи на экранах владельца (зеркало web/src/lib/payment.ts): главное число —
 * цена услуги ([Booking.price]); оплата онлайн ([Payment.amount]) — отдельной строкой.
 */
object BookingMoney {
    const val BY_PACKAGE = "По абонементу"

    /** «3 000 ₽» · «По абонементу» · "" (бесплатно / цены нет). */
    fun priceText(booking: Booking, page: BookingPage?): String {
        if (booking.byPackage) return BY_PACKAGE
        val amount = booking.priceWith(page)
        return if (amount > 0) Money.format(amount, booking.currencyWith(page)) else ""
    }

    /** «Депозит 1 000 ₽» · «Предоплата 3 000 ₽» · «Оплата 3 000 ₽». */
    fun paymentText(payment: Payment): String {
        val what = when (payment.kind) {
            "deposit" -> "Депозит"
            "full" -> "Предоплата"
            else -> "Оплата"
        }
        return "$what ${Money.format(payment.amount, payment.currency)}"
    }

    /**
     * Сумма встреч дня («4 встречи · 6 000 ₽»): цены активных записей без абонемента
     * (занятия по абонементу оплачены пакетом); одна валюта — первая встреченная.
     */
    fun dayTotal(bookings: List<Booking>, pages: List<BookingPage>?): Pair<Long, String> {
        var amount = 0L
        var currency = ""
        for (booking in bookings) {
            if (!booking.active || booking.byPackage) continue
            val page = pages.orEmpty().firstOrNull { it.id == booking.pageId }
            val price = booking.priceWith(page)
            if (price <= 0) continue
            val cur = booking.currencyWith(page)
            if (currency.isNotEmpty() && cur != currency) continue
            currency = cur
            amount += price
        }
        return amount to currency
    }
}

/** Свободное окно страницы (вид гостя `booking.get` → `slots[]`): владелец видит те же окна. */
data class BookingSlot(val start: String, val end: String, val day: String = "")

/** Пост «свободные окна» (`booking.slotsPost`, F6). */
data class SlotsPost(val text: String, val slots: Int, val url: String) {
    companion object {
        const val FORMAT_TELEGRAM = "telegram"
        const val FORMAT_VK = "vk"
        const val FORMAT_PLAIN = "plain"
        val FORMATS = listOf(FORMAT_TELEGRAM, FORMAT_VK, FORMAT_PLAIN)

        fun from(json: JsonObject?): SlotsPost = SlotsPost(json.str("text"), json.long("slots").toInt(), json.str("url"))
    }
}

/** Пакет (абонемент) глазами владельца (F5). */
data class PackagePurchase(
    val id: Long,
    val pageId: Long,
    val pageTitle: String,
    /** 0 — выдан вне предложений страницы. */
    val offerId: Long,
    val title: String,
    val sessionsTotal: Int,
    val sessionsUsed: Int,
    val sessionsLeft: Int,
    val validDays: Int,
    /** "" — ещё не оплачен / не активирован. */
    val validUntil: String,
    val price: Long,
    val currency: String,
    /** pending | active | used | expired | cancelled */
    val status: String,
    val payment: Payment?,
    val guestName: String,
    val guestEmail: String,
    val ccEmail: String,
    val createdAt: String,
) {
    /** Можно ли менять остаток и отменять (отменённый сервер не правит). */
    val editable get() = status != CANCELLED

    companion object {
        const val PENDING = "pending"
        const val ACTIVE = "active"
        const val USED = "used"
        const val EXPIRED = "expired"
        const val CANCELLED = "cancelled"
        val STATUSES = listOf(PENDING, ACTIVE, USED, EXPIRED, CANCELLED)

        fun from(json: JsonObject?): PackagePurchase = PackagePurchase(
            id = json.long("id"),
            pageId = json.long("page_id"),
            pageTitle = json.str("page_title"),
            offerId = json.long("offer_id"),
            title = json.str("title"),
            sessionsTotal = json.long("sessions_total").toInt(),
            sessionsUsed = json.long("sessions_used").toInt(),
            sessionsLeft = json.long("sessions_left").toInt(),
            validDays = json.long("valid_days").toInt(),
            validUntil = json.str("valid_until"),
            price = json.long("price"),
            currency = json.str("currency").ifEmpty { "RUB" },
            status = json.str("status"),
            payment = Payment.from(json.obj("payment")),
            guestName = json.str("guest_name"),
            guestEmail = json.str("guest_email"),
            ccEmail = json.str("cc_email"),
            createdAt = json.str("created_at"),
        )
    }
}

/**
 * Выдача пакета вне сайта (`package.issue`): по предложению страницы ([offerId])
 * или произвольный ([title] + [sessions] + цена и срок).
 */
data class PackageIssueRequest(
    val pageId: Long,
    val guestName: String,
    val offerId: Long = 0,
    val title: String = "",
    val sessions: Int = 0,
    val price: Long = 0,
    val validDays: Int = 90,
    val guestEmail: String = "",
    val ccEmail: String = "",
    val paid: Boolean = false,
) {
    fun validate(): String? {
        val name = guestName.trim()
        val custom = title.trim()
        return when {
            pageId <= 0 -> "Выберите страницу записи"
            name.isEmpty() || name.codePointCount(0, name.length) > 80 -> "Имя клиента — от 1 до 80 символов"
            offerId <= 0 && (custom.isEmpty() || custom.codePointCount(0, custom.length) > PackageOffer.TITLE_MAX) ->
                "Назовите пакет — до ${PackageOffer.TITLE_MAX} символов"
            offerId <= 0 && sessions !in 1..100 -> "Занятий — от 1 до 100"
            offerId <= 0 && (price < 0 || price > Money.PRICE_MAX) -> "Неверная цена"
            offerId <= 0 && validDays !in 7..365 -> "Срок — от 7 до 365 дней"
            guestEmail.trim().length > 254 || ccEmail.trim().length > 254 -> "Email — до 254 символов"
            ccEmail.isNotBlank() && ccEmail.trim().equals(guestEmail.trim(), ignoreCase = true) -> "Копия — на другой адрес"
            else -> null
        }
    }

    fun toJson(): JsonObject = buildJsonObject {
        put("page_id", pageId)
        if (offerId > 0) {
            put("offer_id", offerId)
        } else {
            put("title", title.trim())
            put("sessions", sessions)
            put("price", price)
            put("valid_days", validDays)
        }
        put("guest_name", guestName.trim())
        if (guestEmail.isNotBlank()) put("guest_email", guestEmail.trim())
        if (ccEmail.isNotBlank()) put("cc_email", ccEmail.trim())
        put("paid", paid)
    }
}

/** Ответ `package.issue`: токен ссылки пакета показывается один раз. */
data class PackageIssued(val purchase: PackagePurchase, val token: String, val url: String) {
    /** Ссылка для клиента: от сервера или путь веб-клиента. */
    val link: String get() = url.ifEmpty { if (token.isNotEmpty()) "#/p/$token" else "" }
}

data class Referral(val displayName: String, val source: String, val joinedAt: String, val verified: Boolean)

data class ReferralInfo(
    val code: String,
    /** Готовая ссылка или "" — если на сервере не задан SELFVOY_PUBLIC_URL. */
    val url: String,
    val joined: Int,
    val verified: Int,
    val referrals: List<Referral>,
    /** F3: сколько месяцев Pro начислено пригласившему (≤ [rewardCap]). */
    val rewarded: Int = 0,
    val rewardDays: Int = 30,
    val rewardCap: Int = 12,
    val plan: Plan? = null,
) {
    /** Есть ли полная ссылка; без SELFVOY_PUBLIC_URL делимся только кодом. */
    val hasLink: Boolean get() = url.isNotEmpty()

    /**
     * Что показать и скопировать: полная ссылка или (без SELFVOY_PUBLIC_URL) сам код —
     * голый путь «#/signup?ref=…» без адреса веб-клиента никуда не ведёт.
     */
    val link: String get() = url.ifEmpty { code }

    /** Текст для «Поделиться»: ссылка или код, который друг введёт при регистрации. */
    val shareText: String get() = if (hasLink) "Заведи свой Selfvoy: $url" else "Код приглашения в Selfvoy: $code"

    /** Сколько ещё наград можно получить. */
    val rewardsLeft: Int get() = (rewardCap - rewarded).coerceAtLeast(0)

    companion object {
        fun from(json: JsonObject?): ReferralInfo = ReferralInfo(
            code = json.str("code"),
            url = json.str("url"),
            joined = json.long("joined").toInt(),
            verified = json.long("verified").toInt(),
            referrals = json.arr("referrals").mapNotNull { element ->
                val item = element as? JsonObject ?: return@mapNotNull null
                Referral(item.str("display_name"), item.str("source"), item.str("joined_at"), item.bool("verified"))
            },
            rewarded = json.long("rewarded").toInt(),
            rewardDays = json.long("reward_days").toInt().takeIf { it > 0 } ?: 30,
            rewardCap = json.long("reward_cap").toInt().takeIf { it > 0 } ?: 12,
            plan = Plan.from(json.obj("plan")),
        )
    }
}

/** Реферальный код: 8 символов A–Z/0–9 (сервер сравнивает в верхнем регистре). */
object ReferralCode {
    const val LENGTH = 8
    private val pattern = Regex("^[A-Z0-9]{$LENGTH}$")

    /** Нормализованный код или null — неверный код не отправляем (сервер его всё равно молча проигнорирует). */
    fun normalize(raw: String?): String? {
        val code = raw?.trim()?.uppercase() ?: return null
        return if (pattern.matches(code)) code else null
    }
}
