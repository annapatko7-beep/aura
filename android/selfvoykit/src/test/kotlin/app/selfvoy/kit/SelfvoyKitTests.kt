package app.selfvoy.kit

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// SelfvoyKitTests — тесты общего слоя без сети: кадры протокола, deep links,
// доступы к JsonObject. Запуск: ./gradlew :selfvoykit:test (JVM, без эмулятора).
// Зеркало ios/SelfvoyKit/Tests/SelfvoyKitTests/SelfvoyKitTests.swift.

class ProtocolFrameTests {

    private val json = Json { ignoreUnknownKeys = true }

    @Test
    fun `request frame shape`() {
        val frame = SelfvoyRequestFrame(
            id = "1",
            type = "auth.login",
            payload = buildJsonObject { put("email", "a@b.c") },
        )
        val encoded = json.encodeToString(SelfvoyRequestFrame.serializer(), frame)
        val decoded = json.parseToJsonElement(encoded)
        val obj = decoded as kotlinx.serialization.json.JsonObject
        assertEquals("1", (obj["id"] as kotlinx.serialization.json.JsonPrimitive).content)
        assertEquals("auth.login", (obj["type"] as kotlinx.serialization.json.JsonPrimitive).content)
        val payload = obj["payload"] as kotlinx.serialization.json.JsonObject
        assertEquals("a@b.c", payload.str("email"))
    }

    @Test
    fun `error frame decoding`() {
        val frame = json.decodeFromString(
            SelfvoyResponseFrame.serializer(),
            """{"id":"1","type":"error","code":"bad_request","message":"нужен email"}""",
        )
        assertEquals("error", frame.type)
        assertEquals("bad_request", frame.code)
        assertEquals("нужен email", frame.message)
    }

    @Test
    fun `event frame decoding`() {
        val frame = json.decodeFromString(
            SelfvoyResponseFrame.serializer(),
            """{"type":"event","event":"chat.message","payload":{"body":"ок"}}""",
        )
        assertEquals("event", frame.type)
        assertEquals("chat.message", frame.event)
        assertEquals("ок", frame.payload.str("body"))
        assertNull(frame.id)
    }

    @Test
    fun `event names match protocol`() {
        // Имена событий — часть протокола (docs/PROTOCOL.md); сверяются здесь,
        // чтобы переименование на клиенте не разъехалось с сервером.
        assertEquals("chat.message", SelfvoyEventName.CHAT_MESSAGE)
        assertEquals("task.due", SelfvoyEventName.TASK_DUE)
        assertEquals("notification.new", SelfvoyEventName.NOTIFICATION_NEW)
    }

    @Test
    fun `json accessors are null safe`() {
        val value: kotlinx.serialization.json.JsonObject? = null
        assertEquals("", value.str("missing"))
        assertEquals(0L, value.long("missing"))
        assertEquals(false, value.bool("missing"))
        assertTrue(value.arr("missing").isEmpty())

        val obj = buildJsonObject { put("n", 12); put("s", "привет"); put("nil", JsonNull) }
        assertEquals(12L, obj.long("n"))
        assertEquals("привет", obj.str("s"))
        assertEquals("", obj.str("nil"))
    }
}

class DeepLinkTests {

    @Test
    fun `chats route with and without id`() {
        assertEquals(DeepLink.Chats(42), DeepLink.parse("selfvoy://chats/42"))
        assertEquals(DeepLink.Chats(null), DeepLink.parse("selfvoy://chats"))
        assertEquals(DeepLink.Chats(null), DeepLink.parse("selfvoy://chats/не-число"))
    }

    @Test
    fun `simple routes`() {
        assertEquals(DeepLink.Voice, DeepLink.parse("selfvoy://voice"))
        assertEquals(DeepLink.Tasks, DeepLink.parse("selfvoy://tasks"))
        assertEquals(DeepLink.Notifications, DeepLink.parse("selfvoy://notifications"))
        assertEquals(DeepLink.Settings, DeepLink.parse("selfvoy://settings"))
    }

    @Test
    fun `ask route decodes query`() {
        assertEquals(
            DeepLink.Ask("привет, Selfvoy"),
            DeepLink.parse("selfvoy://ask?text=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82%2C%20Selfvoy"),
        )
        assertEquals(DeepLink.Ask(""), DeepLink.parse("selfvoy://ask"))
    }

    @Test
    fun `oauth callback requires all three params`() {
        assertEquals(
            DeepLink.OAuth("google_calendar", "code-1", "state-1"),
            DeepLink.parse("selfvoy://oauth?provider=google_calendar&code=code-1&state=state-1"),
        )
        assertNull(DeepLink.parse("selfvoy://oauth?provider=google_calendar&code=code-1"))
    }

    @Test
    fun `signup route keeps only a valid referral code`() {
        assertEquals(DeepLink.Signup("ABCD2345"), DeepLink.parse("selfvoy://signup?ref=abcd2345"))
        assertEquals(DeepLink.Signup(""), DeepLink.parse("selfvoy://signup?ref=bad"))
        assertEquals(DeepLink.Signup(""), DeepLink.parse("selfvoy://signup"))
    }

    @Test
    fun `rejects foreign scheme and unknown host`() {
        assertNull(DeepLink.parse("https://selfvoy.app/chats/1"))
        assertNull(DeepLink.parse("selfvoy://unknown"))
        assertNull(DeepLink.parse("не url"))
    }
}
