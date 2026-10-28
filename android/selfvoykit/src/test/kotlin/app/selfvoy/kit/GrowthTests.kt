package app.selfvoy.kit

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.time.Instant

// Функции роста F1–F6 на стороне владельца: разбор ответов и форма запросов
// (docs/PROTOCOL.md). Транспорт подменён записывающим фейком, как в SelfvoyClientTests.

private class GrowthTransport(
    private val reply: (type: String, payload: JsonObject) -> JsonObject?,
) : WebSocketTransport() {
    val sent = mutableListOf<Pair<String, JsonObject>>()

    override suspend fun request(type: String, payload: JsonObject): JsonObject? {
        sent += type to payload
        return reply(type, payload)
    }
}

private fun obj(text: String): JsonObject = Json.parseToJsonElement(text) as JsonObject

private suspend fun expectBadRequest(block: suspend () -> Unit) {
    try {
        block()
        fail("ожидалась SelfvoyError bad_request")
    } catch (error: SelfvoyError) {
        assertEquals("bad_request", error.code)
    }
}

class PlanTests {

    private val now = Instant.parse("2026-10-16T12:00:00Z")

    @Test
    fun `user carries plan and pro is honest about until`() {
        val user = SelfvoyUser.from(obj("""{"id":1,"email":"a@b.c","display_name":"А",
            "plan":{"name":"pro","until":"2026-11-12T00:00:00Z","enforced":true}}"""))
        val plan = user.plan!!
        assertTrue(plan.isPro(now))
        assertFalse(plan.proLocked(now))
        assertFalse(plan.isPro(Instant.parse("2026-12-01T00:00:00Z")))
        assertTrue(plan.proLocked(Instant.parse("2026-12-01T00:00:00Z")))
    }

    @Test
    fun `free in beta is not locked and old server has no plan`() {
        val beta = Plan.from(obj("""{"name":"free","until":"","enforced":false}"""))!!
        assertFalse(beta.isPro(now))
        assertFalse(beta.proLocked(now))
        assertNull(SelfvoyUser.from(obj("""{"id":1}""")).plan)
        assertNull(Plan.from(null))
    }

    @Test
    fun `pro_required is recognized and explained`() = runBlocking {
        val transport = GrowthTransport { _, _ -> throw SelfvoyError("pro_required", "нужен Pro") }
        try {
            SelfvoyClient(transport).bookingPageUpdate(3, BookingPageFields("Урок", hideBranding = true))
            fail("ожидалась ошибка")
        } catch (error: SelfvoyError) {
            assertTrue(error.isProRequired)
        }
        assertFalse(SelfvoyError("forbidden", "x").isProRequired)
        assertTrue(SelfvoyErrorCode.PRO_REQUIRED_TEXT.contains("месяц Pro"))
    }
}

class MoneyTests {

    @Test
    fun `formats minor units with non-breaking groups`() {
        assertEquals("2 000 ₽", Money.format(200_000))
        assertEquals("2 000,50 ₽", Money.format(200_050, "RUB"))
        assertEquals("15 $", Money.format(1_500, "USD"))
        assertEquals("1 000 000 €", Money.format(100_000_000, "EUR"))
        assertEquals("0 ₽", Money.format(0))
    }

    @Test
    fun `parses owner input`() {
        assertEquals(200_000L, Money.parseInput("2000"))
        assertEquals(200_050L, Money.parseInput("2 000,5"))
        assertEquals(199L, Money.parseInput("1.99"))
        assertEquals(0L, Money.parseInput(""))
        assertEquals(250_000L, Money.parseInput("2500 ₽"))
        assertNull(Money.parseInput("-5"))
        assertNull(Money.parseInput("1,999"))
        assertNull(Money.parseInput("abc"))
    }

    @Test
    fun `input value round trips`() {
        assertEquals("2000", Money.inputValue(200_000))
        assertEquals("2000,50", Money.inputValue(200_050))
        assertEquals("", Money.inputValue(0))
        assertEquals(200_050L, Money.parseInput(Money.inputValue(200_050)))
    }
}

class PageFieldsTests {

    private val pageJson = """{"page":{"id":3,"slug":"abc123def4","url":"https://s.example/#/b/abc123def4",
        "title":"Урок","description":"","duration_minutes":60,"days_ahead":14,"max_per_day":4,"notice_hours":2,
        "max_series":1,"weekends":false,"status":"active","upcoming":2,"guest_reminders":false,"hide_branding":true,
        "price":250000,"currency":"RUB","prepay":"deposit","deposit":50000,"payment_url":"https://pay.example/x",
        "payment_note":"СБП +7 900","pay_within_minutes":30,"cancel_hours":12,"late_cancel":"block",
        "policy_text":"Отмена за 12 часов","packages":[{"id":1,"title":"10 занятий","sessions":10,"price":2000000,"valid_days":90}]}}"""

    @Test
    fun `parses all page fields`() = runBlocking {
        val transport = GrowthTransport { _, _ -> JsonObject(mapOf("pages" to JsonArray(listOf(obj(pageJson).obj("page")!!)))) }
        val page = SelfvoyClient(transport).bookingPageList().single()
        assertFalse(page.guestReminders)
        assertTrue(page.hideBranding)
        assertEquals(250_000L, page.price)
        assertEquals("deposit", page.prepay)
        assertEquals(50_000L, page.deposit)
        assertEquals(30, page.payWithinMinutes)
        assertEquals(12, page.cancelHours)
        assertEquals("block", page.lateCancel)
        assertEquals(PackageOffer(1, "10 занятий", 10, 2_000_000, 90), page.packages.single())
        // Округление: поля формы совпадают со страницей.
        assertEquals(page.packages, page.fields.packages)
        assertNull(page.fields.validate())
    }

    @Test
    fun `old server page gets safe defaults`() {
        val page = BookingPage.from(obj("""{"id":1,"slug":"s","title":"X","status":"active"}"""))
        assertTrue(page.guestReminders)
        assertEquals("none", page.prepay)
        assertEquals("RUB", page.currency)
        assertEquals("allow", page.lateCancel)
        assertEquals(60, page.payWithinMinutes)
        assertTrue(page.packages.isEmpty())
    }

    @Test
    fun `update sends all fields and packages as a whole array`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj(pageJson) }
        val fields = BookingPageFields(
            "Урок", price = 250_000, prepay = "full", paymentNote = " СБП ",
            packages = listOf(PackageOffer(1, "10 занятий", 10, 2_000_000), PackageOffer(title = "5 занятий", sessions = 5, price = 1_100_000)),
        )
        SelfvoyClient(transport).bookingPageUpdate(3, fields)
        val (type, payload) = transport.sent.single()
        assertEquals("booking.pageUpdate", type)
        assertEquals(BookingPageFields.KEYS + "page_id", payload.keys)
        assertEquals("СБП", payload.str("payment_note"))
        assertEquals(0L, payload.long("deposit"))
        val packages = payload["packages"] as JsonArray
        assertEquals(2, packages.size)
        assertEquals(1L, (packages[0] as JsonObject).long("id"))
        // Новое предложение уходит без id — его выдаст сервер.
        assertFalse((packages[1] as JsonObject).containsKey("id"))
    }

    @Test
    fun `invalid payment and package fields fail locally`() = runBlocking {
        val transport = GrowthTransport { _, _ -> fail("запрос не должен уходить"); null }
        val bad = listOf(
            BookingPageFields("X", prepay = "full"),
            BookingPageFields("X", price = 1000, prepay = "deposit", deposit = 2000),
            BookingPageFields("X", paymentUrl = "http://pay.example"),
            BookingPageFields("X", paymentNote = "строка\nвторая"),
            BookingPageFields("X", payWithinMinutes = 10),
            BookingPageFields("X", cancelHours = 200),
            BookingPageFields("X", lateCancel = "maybe"),
            BookingPageFields("X", currency = "GBP"),
            BookingPageFields("X", policyText = "x".repeat(501)),
            BookingPageFields("X", packages = List(6) { PackageOffer(title = "П$it", sessions = 5, price = 100) }),
            BookingPageFields("X", packages = listOf(PackageOffer(title = "П", sessions = 1, price = 100))),
            BookingPageFields("X", packages = listOf(PackageOffer(title = "П", sessions = 5, price = 0))),
        )
        for (fields in bad) expectBadRequest { SelfvoyClient(transport).bookingPageCreate(fields) }
        assertTrue(transport.sent.isEmpty())
        assertNull(BookingPageFields("X", price = 1000, prepay = "deposit", deposit = 500, paymentUrl = "https://pay.example").validate())
    }
}

class PaymentTests {

    private val bookingJson = """{"booking":{"id":9,"page_id":3,"page_title":"Урок","guest_name":"Мария",
        "guest_email":"m@x.ru","guest_comment":"","cc_email":"mom@x.ru","package_id":0,"status":"active",
        "cancelled_by":"","reschedules":0,"created_at":"2026-10-16T08:00:00Z","updated_at":"2026-10-16T08:00:00Z",
        "occurrences":[{"start":"2026-10-17T09:00:00Z","end":"2026-10-17T10:00:00Z","local":"2026-10-17T12:00",
        "status":"active","confirmed_at":"2026-10-16T09:30:00Z"}],
        "reminders":[{"start":"2026-10-17T09:00:00Z","kind":"24h","due_at":"2026-10-16T09:00:00Z","sent_at":"2026-10-16T09:00:05Z"},
        {"start":"2026-10-17T09:00:00Z","kind":"2h","due_at":"2026-10-17T07:00:00Z","sent_at":""}],
        "payment":{"status":"claimed","amount":200000,"currency":"RUB","kind":"full","method":"link",
        "action_url":"https://pay.example/x","due_at":"2026-10-16T09:00:00Z","paid_at":"","refunded_at":""}}}"""

    @Test
    fun `booking parses payment reminders confirmation and cc`() {
        val booking = Booking.from(obj(bookingJson).obj("booking"))
        assertEquals("mom@x.ru", booking.ccEmail)
        assertTrue(booking.occurrences.single().confirmed)
        assertEquals(2, booking.reminders.size)
        assertTrue(booking.reminders.first().sent)
        assertFalse(booking.reminders.last().sent)
        val payment = booking.payment!!
        assertEquals(200_000L, payment.amount)
        assertTrue(payment.awaiting)
        assertEquals(listOf("paid", "waived"), payment.ownerActions)
        assertEquals("2026-10-16T08:00:00Z", booking.createdAt)
    }

    @Test
    fun `null payment and owner actions per status`() {
        assertNull(Booking.from(obj("""{"id":1,"payment":null}""")).payment)
        assertNull(Payment.from(null))
        fun actions(status: String) = Payment(status, 100, "RUB").ownerActions
        assertEquals(listOf("refunded"), actions("paid"))
        assertEquals(listOf("paid"), actions("waived"))
        assertEquals(listOf("paid"), actions("expired"))
        assertTrue(actions("refunded").isEmpty())
    }

    @Test
    fun `paymentSet sends booking id and status`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj(bookingJson.replace("\"claimed\"", "\"paid\"")) }
        val booking = SelfvoyClient(transport).bookingPaymentSet(9, "paid")
        val (type, payload) = transport.sent.single()
        assertEquals("booking.paymentSet", type)
        assertEquals(mapOf("booking_id" to JsonPrimitive(9L), "status" to JsonPrimitive("paid")), payload.toMap())
        assertEquals("paid", booking.payment?.status)
        expectBadRequest { SelfvoyClient(transport).bookingPaymentSet(9, "claimed") }
        assertEquals(1, transport.sent.size)
    }

    @Test
    fun `cancelled by system is kept`() {
        val booking = Booking.from(obj("""{"id":2,"status":"cancelled","cancelled_by":"system"}"""))
        assertEquals("system", booking.cancelledBy)
        assertFalse(booking.active)
    }

    // Цена записи (price/currency) — отдельно от суммы оплаты онлайн (payment.amount).

    private fun page(id: Long, price: Long, currency: String = "RUB") =
        BookingPage.from(obj("""{"id":$id,"slug":"s$id","title":"Стр $id","price":$price,"currency":"$currency","status":"active"}"""))

    @Test
    fun `booking price is parsed apart from payment amount`() {
        val booking = Booking.from(obj("""{"id":3,"page_id":1,"status":"active","price":300000,"currency":"RUB",
            "payment":{"status":"pending","amount":100000,"currency":"RUB","kind":"deposit"}}"""))
        assertEquals(300_000L, booking.price)
        assertEquals("RUB", booking.currency)
        assertEquals(100_000L, booking.payment?.amount)
        val pages = listOf(page(1, 250_000))
        assertEquals(300_000L, booking.priceWith(pages.first()))
        assertEquals(Money.format(300_000), BookingMoney.priceText(booking, pages.first()))
        assertEquals("Депозит ${Money.format(100_000)}", BookingMoney.paymentText(booking.payment!!))
        assertEquals("Предоплата ${Money.format(300_000)}",
            BookingMoney.paymentText(Payment("paid", 300_000, "RUB", kind = "full")))
        assertEquals("Оплата ${Money.format(300_000)}", BookingMoney.paymentText(Payment("paid", 300_000, "RUB")))
    }

    @Test
    fun `old server without price falls back to page price`() {
        val booking = Booking.from(obj("""{"id":3,"page_id":1,"status":"active"}"""))
        assertNull(booking.price)
        assertEquals("", booking.currency)
        assertEquals(250_000L, booking.priceWith(page(1, 250_000, "EUR")))
        assertEquals("EUR", booking.currencyWith(page(1, 250_000, "EUR")))
        assertEquals(0L, booking.priceWith(null))
        assertEquals("RUB", booking.currencyWith(null))
        assertEquals("", BookingMoney.priceText(booking, null))
        // null — как отсутствие поля.
        assertNull(Booking.from(obj("""{"id":3,"price":null}""")).price)
    }

    @Test
    fun `zero price and package sessions do not fall back to page`() {
        val free = Booking.from(obj("""{"id":4,"page_id":1,"status":"active","price":0,"currency":"RUB"}"""))
        assertEquals(0L, free.priceWith(page(1, 250_000)))
        assertEquals("", BookingMoney.priceText(free, page(1, 250_000)))
        val pack = Booking.from(obj("""{"id":5,"page_id":1,"status":"active","package_id":7,"price":0}"""))
        assertTrue(pack.byPackage)
        assertEquals(BookingMoney.BY_PACKAGE, BookingMoney.priceText(pack, page(1, 250_000)))
        // Абонемент без поля price (старый сервер) — тоже не цена страницы.
        val oldPack = Booking.from(obj("""{"id":6,"page_id":1,"status":"active","package_id":7}"""))
        assertEquals(0L, oldPack.priceWith(page(1, 250_000)))
        assertEquals("По абонементу", BookingMoney.priceText(oldPack, null))
    }

    @Test
    fun `day total sums booking prices without package sessions`() {
        fun b(id: Long, json: String) = Booking.from(obj("""{"id":$id,"page_id":1,"status":"active"$json}"""))
        val pages = listOf(page(1, 250_000))
        val bookings = listOf(
            b(1, ""","price":300000,"currency":"RUB","payment":{"status":"pending","amount":100000,"kind":"deposit"}"""),
            b(2, ""","price":300000,"currency":"RUB""""),
            b(3, ""","price":0,"currency":"RUB","package_id":4"""),
            b(4, ""","price":0,"currency":"RUB""""),
            b(5, ""), // старый сервер — цена страницы
            b(6, ""","price":900000,"currency":"USD""""), // другая валюта не складывается
            Booking.from(obj("""{"id":7,"page_id":1,"status":"cancelled","price":300000,"currency":"RUB"}""")),
        )
        assertEquals(850_000L to "RUB", BookingMoney.dayTotal(bookings, pages))
        assertEquals(0L to "", BookingMoney.dayTotal(listOf(bookings[2], bookings[3]), pages))
        assertEquals(0L to "", BookingMoney.dayTotal(emptyList(), null))
    }
}

class PackageTests {

    private val packageJson = """{"id":5,"page_id":3,"page_title":"Урок","offer_id":1,"title":"10 занятий",
        "sessions_total":10,"sessions_used":3,"sessions_left":7,"valid_days":90,"valid_until":"2027-01-14T00:00:00Z",
        "price":2000000,"currency":"RUB","status":"active","payment":{"status":"pending","amount":2000000,
        "currency":"RUB","kind":"package","method":"link","action_url":"","due_at":"","paid_at":"","refunded_at":""},
        "guest_name":"Сергей","guest_email":"s@x.ru","cc_email":"","created_at":"2026-10-01T08:00:00Z","updated_at":""}"""

    @Test
    fun `list parses packages and sends filters`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj("""{"packages":[$packageJson]}""") }
        val list = SelfvoyClient(transport).packageList(pageId = 3, status = "active")
        val (type, payload) = transport.sent.single()
        assertEquals("package.list", type)
        assertEquals(setOf("page_id", "status", "limit"), payload.keys)
        val item = list.single()
        assertEquals(7, item.sessionsLeft)
        assertEquals("package", item.payment?.kind)
        assertTrue(item.editable)

        SelfvoyClient(transport).packageList()
        assertEquals(setOf("limit"), transport.sent.last().second.keys)
        expectBadRequest { SelfvoyClient(transport).packageList(status = "lost") }
    }

    @Test
    fun `issue by offer or custom and token link`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj("""{"package":$packageJson,"token":"tok","url":""}""") }
        val client = SelfvoyClient(transport)
        val issued = client.packageIssue(PackageIssueRequest(pageId = 3, guestName = " Сергей ", offerId = 1, guestEmail = "s@x.ru", paid = true))
        val byOffer = transport.sent.last().second
        assertEquals(setOf("page_id", "offer_id", "guest_name", "guest_email", "paid"), byOffer.keys)
        assertEquals("Сергей", byOffer.str("guest_name"))
        assertEquals("#/p/tok", issued.link)
        assertEquals(5L, issued.purchase.id)

        client.packageIssue(PackageIssueRequest(pageId = 3, guestName = "Аня", title = "Разовый", sessions = 4, price = 800_000))
        assertEquals(setOf("page_id", "title", "sessions", "price", "valid_days", "guest_name", "paid"), transport.sent.last().second.keys)

        expectBadRequest { client.packageIssue(PackageIssueRequest(pageId = 3, guestName = "")) }
        expectBadRequest { client.packageIssue(PackageIssueRequest(pageId = 3, guestName = "Аня", title = "П", sessions = 0)) }
        expectBadRequest { client.packageIssue(PackageIssueRequest(pageId = 3, guestName = "Аня", offerId = 1, guestEmail = "a@x.ru", ccEmail = "A@x.ru")) }
        assertEquals(2, transport.sent.size)
    }

    @Test
    fun `adjust cancel and paymentSet`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj("""{"package":$packageJson}""") }
        val client = SelfvoyClient(transport)
        client.packageAdjust(5, -2)
        assertEquals("package.adjust", transport.sent.last().first)
        assertEquals(mapOf("package_id" to JsonPrimitive(5L), "delta" to JsonPrimitive(-2)), transport.sent.last().second.toMap())
        client.packageCancel(5)
        assertEquals("package.cancel" to setOf("package_id"), transport.sent.last().first to transport.sent.last().second.keys)
        client.packagePaymentSet(5, "waived")
        assertEquals("package.paymentSet", transport.sent.last().first)
        assertEquals("waived", transport.sent.last().second.str("status"))
        expectBadRequest { client.packageAdjust(5, 0) }
        expectBadRequest { client.packageAdjust(5, 101) }
        expectBadRequest { client.packagePaymentSet(5, "pending") }
        assertEquals(3, transport.sent.size)
    }
}

class SlotsAndCalendarTests {

    @Test
    fun `slots post request and reply`() = runBlocking {
        val transport = GrowthTransport { _, _ -> obj("""{"text":"Свободные окна: Урок","slots":5,"url":""}""") }
        val post = SelfvoyClient(transport).bookingSlotsPost(3, days = 14, format = "vk")
        assertEquals("booking.slotsPost", transport.sent.single().first)
        assertEquals(mapOf("page_id" to JsonPrimitive(3L), "days" to JsonPrimitive(14), "format" to JsonPrimitive("vk")),
            transport.sent.single().second.toMap())
        assertEquals(SlotsPost("Свободные окна: Урок", 5, ""), post)
        expectBadRequest { SelfvoyClient(transport).bookingSlotsPost(3, days = 15) }
        expectBadRequest { SelfvoyClient(transport).bookingSlotsPost(3, format = "html") }
    }

    @Test
    fun `free slots come from guest view of own page`() = runBlocking {
        val transport = GrowthTransport { _, _ ->
            obj("""{"page":{"slug":"abc","slots":[{"start":"2026-10-17T08:00:00Z","end":"2026-10-17T09:00:00Z","day":"2026-10-17"}]}}""")
        }
        val slots = SelfvoyClient(transport).bookingFreeSlots("abc")
        assertEquals("booking.get", transport.sent.single().first)
        assertEquals(BookingSlot("2026-10-17T08:00:00Z", "2026-10-17T09:00:00Z", "2026-10-17"), slots.single())
    }

    @Test
    fun `feed url is built from server address when url is empty`() {
        val feed = CalendarFeed(true, "", "/ics/TOKEN.ics")
        assertEquals("https://api.example.com/ics/TOKEN.ics", feed.feedUrl("wss://api.example.com"))
        assertEquals("http://10.0.2.2:9000/ics/TOKEN.ics", feed.feedUrl("ws://10.0.2.2:9000/"))
        assertEquals("https://x.example/ics/T.ics", CalendarFeed(true, "https://x.example/ics/T.ics", "/ics/T.ics").feedUrl("ws://other"))
        assertNull(CalendarFeed(true, "", "").feedUrl("wss://api.example.com"))
        assertNull(feed.feedUrl("api.example.com"))
        assertEquals("webcal://api.example.com/ics/T.ics", CalendarFeed.webcal("https://api.example.com/ics/T.ics"))
        assertNull(CalendarFeed.webcal("http://api.example.com/ics/T.ics"))
    }

    @Test
    fun `feed methods`() = runBlocking {
        val transport = GrowthTransport { type, _ ->
            when (type) {
                "calendar.feedReset" -> obj("""{"enabled":true,"url":"","path":"/ics/abc.ics"}""")
                "calendar.feedDisable" -> obj("""{"enabled":false}""")
                else -> obj("""{"enabled":true,"url":"","path":""}""")
            }
        }
        val client = SelfvoyClient(transport)
        assertTrue(client.calendarFeed().enabled)
        assertEquals("/ics/abc.ics", client.calendarFeedReset().path)
        assertFalse(client.calendarFeedDisable().enabled)
        assertEquals(listOf("calendar.feed", "calendar.feedReset", "calendar.feedDisable"), transport.sent.map { it.first })
    }
}

class TelegramAndPaymentsTests {

    @Test
    fun `telegram status link unlink`() = runBlocking {
        val transport = GrowthTransport { type, _ ->
            when (type) {
                "telegram.status" -> obj("""{"enabled":true,"bot_username":"selfvoy_bot","linked":true,"username":"anna"}""")
                "telegram.link" -> obj("""{"url":"https://t.me/selfvoy_bot?start=o_1_2_x","expires_at":"2026-10-16T12:15:00Z"}""")
                else -> obj("""{"linked":false}""")
            }
        }
        val client = SelfvoyClient(transport)
        assertEquals(TelegramStatus(true, "selfvoy_bot", true, "anna"), client.telegramStatus())
        assertEquals("https://t.me/selfvoy_bot?start=o_1_2_x", client.telegramLink().url)
        client.telegramUnlink()
        assertEquals(listOf("telegram.status", "telegram.link", "telegram.unlink"), transport.sent.map { it.first })
    }

    @Test
    fun `telegram notifications flag keeps other notification prefs`() = runBlocking {
        val transport = GrowthTransport { _, payload -> payload }
        val current = obj("""{"quiet_hours":{"start":"22:00","end":"08:00"},"muted_kinds":["login.new"]}""")
        val reply = SelfvoyClient(transport).notificationsTelegramSet(false, current)
        val (type, payload) = transport.sent.single()
        assertEquals("prefs.set", type)
        assertEquals(setOf("notifications"), payload.keys)
        val notifications = payload.obj("notifications")!!
        assertEquals(setOf("quiet_hours", "muted_kinds", "telegram"), notifications.keys)
        assertFalse(telegramNotificationsEnabled(reply))
        assertTrue(telegramNotificationsEnabled(obj("""{"notifications":{}}""")))
        assertTrue(telegramNotificationsEnabled(null))
    }

    @Test
    fun `yookassa connect validates and never prints the secret`() = runBlocking {
        val transport = GrowthTransport { type, _ ->
            if (type == "payments.disconnect") obj("""{"connected":false}""")
            else obj("""{"connected":true,"provider":"yookassa","shop_id":"123456","connected_at":"2026-10-16T12:00:00Z"}""")
        }
        val client = SelfvoyClient(transport)
        val credentials = YooKassaCredentials(" 123456 ", " test_SECRETVALUE ")
        assertFalse(credentials.toString().contains("SECRETVALUE"))
        val status = client.paymentsConnect(credentials)
        assertTrue(status.connected)
        val (type, payload) = transport.sent.single()
        assertEquals("payments.connect", type)
        assertEquals(mapOf("provider" to JsonPrimitive("yookassa"), "shop_id" to JsonPrimitive("123456"),
            "secret_key" to JsonPrimitive("test_SECRETVALUE")), payload.toMap())

        expectBadRequest { client.paymentsConnect(YooKassaCredentials("12ab", "test_x")) }
        expectBadRequest { client.paymentsConnect(YooKassaCredentials("123", "secret")) }
        assertEquals(1, transport.sent.size)
        assertFalse(client.paymentsDisconnect().connected)
        assertEquals("123456", PaymentsStatus.from(obj("""{"connected":true,"shop_id":"123456"}""")).shopId)
    }
}

class ReferralRewardTests {

    @Test
    fun `referral carries reward counters and plan`() {
        val info = ReferralInfo.from(obj("""{"code":"ABCD2345","url":"","joined":3,"verified":2,"rewarded":2,
            "reward_days":30,"reward_cap":12,"plan":{"name":"pro","until":"2026-12-16T00:00:00Z","enforced":false},"referrals":[]}"""))
        assertEquals(2, info.rewarded)
        assertEquals(30, info.rewardDays)
        assertEquals(10, info.rewardsLeft)
        assertEquals("pro", info.plan?.name)
    }

    @Test
    fun `old server referral gets default reward terms`() {
        val info = ReferralInfo.from(obj("""{"code":"ABCD2345"}"""))
        assertEquals(0, info.rewarded)
        assertEquals(30, info.rewardDays)
        assertEquals(12, info.rewardCap)
        assertNull(info.plan)
    }

    @Test
    fun `notice kinds route refreshes`() {
        assertTrue(SelfvoyNoticeKind.touchesBookings(SelfvoyNoticeKind.BOOKING_PAYMENT))
        assertTrue(SelfvoyNoticeKind.touchesBookings(SelfvoyNoticeKind.PACKAGE_PURCHASED))
        assertTrue(SelfvoyNoticeKind.touchesPackages(SelfvoyNoticeKind.PACKAGE_LOW))
        assertTrue(SelfvoyNoticeKind.touchesReferral(SelfvoyNoticeKind.REFERRAL_REWARD))
        assertFalse(SelfvoyNoticeKind.touchesReferral("login.new"))
    }

    @Test
    fun `tolerant number and flag helpers`() {
        val json = obj("""{"a":null,"b":"12","c":{"x":1},"d":3.0,"e":null}""")
        assertEquals(0L, json.long("a"))
        assertEquals(12L, json.long("b"))
        assertEquals(0L, json.long("c"))
        assertEquals(3L, json.long("d"))
        assertEquals("", json.str("c"))
        assertTrue(json.boolOr("e", true))
        assertFalse(json.bool("missing"))
    }
}
