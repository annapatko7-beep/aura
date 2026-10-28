package app.selfvoy.kit

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

// SelfvoyClientTests — типизированные методы SelfvoyClient без сети: транспорт
// подменён записывающим фейком (WebSocketTransport открыт ради этого).
// Сверяется форма запроса (type + payload) и разбор ответа сервера.

/** Фейковый транспорт: запоминает запросы и отвечает заготовкой или ошибкой. */
private class RecordingTransport(
    private val reply: (type: String, payload: JsonObject) -> JsonObject?,
) : WebSocketTransport() {
    val sent = mutableListOf<Pair<String, JsonObject>>()

    override suspend fun request(type: String, payload: JsonObject): JsonObject? {
        sent += type to payload
        return reply(type, payload)
    }
}

private fun json(text: String): JsonObject = Json.parseToJsonElement(text) as JsonObject

class ProfileUpdateTests {

    @Test
    fun `sends only passed fields and parses user with bio`() = runBlocking {
        val transport = RecordingTransport { _, _ ->
            json("""{"user":{"id":7,"email":"a@b.c","display_name":"Аня","bio":"Люблю кофе","avatar_url":""}}""")
        }
        val user = SelfvoyClient(transport).profileUpdate(displayName = "Аня", bio = "Люблю кофе")

        val (type, payload) = transport.sent.single()
        assertEquals("profile.update", type)
        assertEquals(setOf("display_name", "bio"), payload.keys)
        assertEquals("Аня", payload.str("display_name"))
        assertEquals("Люблю кофе", payload.str("bio"))
        assertEquals(SelfvoyUser(7, "a@b.c", "Аня", "Люблю кофе", ""), user)
    }

    @Test
    fun `bio only and empty avatar are sent as is`() = runBlocking {
        val transport = RecordingTransport { _, _ -> json("""{"user":{"id":1}}""") }
        SelfvoyClient(transport).profileUpdate(bio = "", avatarUrl = "")
        val payload = transport.sent.single().second
        assertEquals(setOf("bio", "avatar_url"), payload.keys)
        assertEquals("", payload.str("bio"))
    }

    @Test
    fun `empty update fails locally without a request`() = runBlocking {
        val transport = RecordingTransport { _, _ -> fail("запрос не должен уходить"); null }
        try {
            SelfvoyClient(transport).profileUpdate()
            fail("ожидалась SelfvoyError")
        } catch (error: SelfvoyError) {
            assertEquals("bad_request", error.code)
        }
        assertTrue(transport.sent.isEmpty())
    }

    @Test
    fun `server bad_request propagates`() = runBlocking {
        val transport = RecordingTransport { _, _ -> throw SelfvoyError("bad_request", "имя — от 1 до 80 символов") }
        try {
            SelfvoyClient(transport).profileUpdate(displayName = "x".repeat(81))
            fail("ожидалась SelfvoyError")
        } catch (error: SelfvoyError) {
            assertEquals("bad_request", error.code)
            assertEquals("имя — от 1 до 80 символов", error.serverMessage)
        }
    }

    @Test
    fun `user without bio parses to empty bio`() {
        val user = SelfvoyUser.from(json("""{"id":3,"email":"x@y.z","display_name":"Икс"}"""))
        assertEquals("", user.bio)
        assertEquals("Икс", user.displayName)
        assertEquals(3L, user.id)
    }
}

class PrivacyTests {

    @Test
    fun `defaults are true when prefs have no privacy`() {
        assertEquals(Privacy(true, true, true), Privacy.fromPrefs(json("""{"theme":"dark"}""")))
        assertEquals(Privacy(), Privacy.fromPrefs(null))
    }

    @Test
    fun `reads flags and fills missing ones with true`() {
        val privacy = Privacy.fromPrefs(json("""{"privacy":{"share_free_slots_only":false,"allow_place_choice":false}}"""))
        assertFalse(privacy.shareFreeSlotsOnly)
        assertTrue(privacy.hideEventTitles)
        assertFalse(privacy.allowPlaceChoice)
    }

    @Test
    fun `privacyGet reads prefs get`() = runBlocking {
        val transport = RecordingTransport { _, _ ->
            json("""{"privacy":{"share_free_slots_only":true,"hide_event_titles":false,"allow_place_choice":true}}""")
        }
        val privacy = SelfvoyClient(transport).privacyGet()
        assertEquals("prefs.get", transport.sent.single().first)
        assertEquals(Privacy(shareFreeSlotsOnly = true, hideEventTitles = false, allowPlaceChoice = true), privacy)
    }

    @Test
    fun `privacySet sends subset at payload root without preferences wrapper`() = runBlocking {
        val transport = RecordingTransport { _, _ ->
            json("""{"onboarded":true,"privacy":{"share_free_slots_only":true,"hide_event_titles":false,"allow_place_choice":true}}""")
        }
        val result = SelfvoyClient(transport).privacySet(hideEventTitles = false)

        val (type, payload) = transport.sent.single()
        assertEquals("prefs.set", type)
        assertEquals(setOf("privacy"), payload.keys)
        val flags = payload.obj("privacy")!!
        assertEquals(setOf("hide_event_titles"), flags.keys)
        assertEquals(JsonPrimitive(false), flags["hide_event_titles"])
        assertEquals(Privacy(true, false, true), result)
    }

    @Test
    fun `privacySet with full model sends all three flags`() = runBlocking {
        val transport = RecordingTransport { _, payload -> buildPrefs(payload) }
        val wanted = Privacy(shareFreeSlotsOnly = false, hideEventTitles = true, allowPlaceChoice = false)
        assertEquals(wanted, SelfvoyClient(transport).privacySet(wanted))
        assertEquals(
            setOf(Privacy.SHARE_FREE_SLOTS_ONLY, Privacy.HIDE_EVENT_TITLES, Privacy.ALLOW_PLACE_CHOICE),
            transport.sent.single().second.obj("privacy")!!.keys,
        )
    }

    /** Эхо сервера: prefs.set возвращает обновлённые настройки целиком. */
    private fun buildPrefs(payload: JsonObject): JsonObject = JsonObject(mapOf("privacy" to payload.obj("privacy")!!))
}

class BookingTests {

    @Test
    fun `page create sends all fields and parses page`() = runBlocking {
        val transport = RecordingTransport { _, _ ->
            json("""{"page":{"id":3,"slug":"abc123def4","url":"","title":"Консультация","description":"",
                "duration_minutes":60,"days_ahead":14,"max_per_day":4,"notice_hours":2,"max_series":3,
                "weekends":false,"status":"active","upcoming":0}}""")
        }
        val page = SelfvoyClient(transport).bookingPageCreate(
            BookingPageFields(" Консультация ", durationMinutes = 60, maxSeries = 3))
        val (type, payload) = transport.sent.single()
        assertEquals("booking.pageCreate", type)
        // Все поля страницы, включая F1/F3/F4/F5 (пакеты — целым массивом).
        assertEquals(BookingPageFields.KEYS, payload.keys)
        assertEquals("Консультация", payload.str("title"))
        assertEquals(60L, payload.long("duration_minutes"))
        assertEquals(3, page.maxSeries)
        assertEquals("#/b/abc123def4", page.link)
    }

    @Test
    fun `invalid page fails locally without a request`() = runBlocking {
        val transport = RecordingTransport { _, _ -> fail("запрос не должен уходить"); null }
        for (bad in listOf(BookingPageFields(""), BookingPageFields("X", durationMinutes = 10),
                BookingPageFields("X", maxSeries = 13), BookingPageFields("X", noticeHours = 169))) {
            try {
                SelfvoyClient(transport).bookingPageCreate(bad)
                fail("ожидалась ошибка для $bad")
            } catch (error: SelfvoyError) {
                assertEquals("bad_request", error.code)
            }
        }
        assertTrue(transport.sent.isEmpty())
    }

    @Test
    fun `pause sends only status`() = runBlocking {
        val transport = RecordingTransport { _, _ -> json("""{"page":{"id":3,"status":"paused"}}""") }
        val page = SelfvoyClient(transport).bookingPageUpdate(3, status = "paused")
        assertEquals(setOf("page_id", "status"), transport.sent.single().second.keys)
        assertEquals("paused", page.status)
    }

    @Test
    fun `bookings and referral are parsed`() = runBlocking {
        val transport = RecordingTransport { type, _ ->
            when (type) {
                "booking.list" -> json("""{"bookings":[{"id":9,"page_id":3,"page_title":"Консультация",
                    "guest_name":"Пётр","guest_email":"p@x.ru","guest_comment":"","status":"active",
                    "cancelled_by":"","reschedules":1,"occurrences":[
                    {"start":"2026-10-28T07:00:00Z","end":"2026-10-28T08:00:00Z","status":"cancelled"},
                    {"start":"2026-10-21T07:00:00Z","end":"2026-10-21T08:00:00Z","status":"active","local":"2026-10-21T10:00"}]}]}""")
                "referral.get" -> json("""{"code":"ABCD2345","url":"","joined":2,"verified":1,
                    "referrals":[{"display_name":"Фёдор","source":"booking","joined_at":"2026-10-14T08:00:00Z","verified":true}]}""")
                else -> json("{}")
            }
        }
        val client = SelfvoyClient(transport)
        val booking = client.bookingList().single()
        assertEquals("2026-10-21T07:00:00Z", booking.occurrences.first().start)
        assertEquals("2026-10-21T10:00", booking.occurrences.first().local)
        assertFalse(booking.occurrences.last().active)
        val referral = client.referralGet()
        // Без SELFVOY_PUBLIC_URL делимся кодом, а не голым «#/signup?ref=…».
        assertFalse(referral.hasLink)
        assertEquals("ABCD2345", referral.link)
        assertEquals("Код приглашения в Selfvoy: ABCD2345", referral.shareText)
        assertEquals(Referral("Фёдор", "booking", "2026-10-14T08:00:00Z", true), referral.referrals.single())
        client.bookingOwnerCancel(9, "2026-10-21T07:00:00Z")
        assertEquals(mapOf("booking_id" to JsonPrimitive(9L), "occurrence" to JsonPrimitive("2026-10-21T07:00:00Z")),
            transport.sent.last().second.toMap())
    }
}

class ReferralTests {

    @Test
    fun `referral with public url shares the link`() {
        val info = ReferralInfo.from(json("""{"code":"ABCD2345","url":"https://selfvoy.example/#/signup?ref=ABCD2345"}"""))
        assertTrue(info.hasLink)
        assertEquals("https://selfvoy.example/#/signup?ref=ABCD2345", info.link)
        assertEquals("Заведи свой Selfvoy: https://selfvoy.example/#/signup?ref=ABCD2345", info.shareText)
    }

    @Test
    fun `code is normalized and validated`() {
        assertEquals("ABCD2345", ReferralCode.normalize(" abcd2345 "))
        assertEquals(null, ReferralCode.normalize(""))
        assertEquals(null, ReferralCode.normalize(null))
        assertEquals(null, ReferralCode.normalize("ABCD234"))
        assertEquals(null, ReferralCode.normalize("ABCD-2345"))
        assertEquals(null, ReferralCode.normalize("АБВГ2345"))
    }

    @Test
    fun `register without ref sends no ref fields`() = runBlocking {
        val transport = RecordingTransport { _, _ -> json("{}") }
        SelfvoyClient(transport).register("a@b.c", "secret123", "Аня")
        val (type, payload) = transport.sent.single()
        assertEquals("auth.register", type)
        assertEquals(setOf("email", "password", "display_name"), payload.keys)
    }

    @Test
    fun `register with ref sends normalized code and link source`() = runBlocking {
        val transport = RecordingTransport { _, _ -> json("{}") }
        SelfvoyClient(transport).register("a@b.c", "secret123", "Аня", ref = " abcd2345")
        val payload = transport.sent.single().second
        assertEquals(setOf("email", "password", "display_name", "ref", "ref_source"), payload.keys)
        assertEquals("ABCD2345", payload.str("ref"))
        assertEquals("link", payload.str("ref_source"))
    }

    @Test
    fun `register drops invalid or blank ref`() = runBlocking {
        val transport = RecordingTransport { _, _ -> json("{}") }
        val client = SelfvoyClient(transport)
        client.register("a@b.c", "secret123", "Аня", ref = "bad")
        client.register("a@b.c", "secret123", "Аня", ref = "  ")
        transport.sent.forEach { (_, payload) ->
            assertEquals(setOf("email", "password", "display_name"), payload.keys)
        }
    }
}
