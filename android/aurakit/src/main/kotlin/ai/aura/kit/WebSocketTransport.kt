package ai.aura.kit

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString

/**
 * WS-транспорт Aura: подключение, конкурентные запросы по id, события сервера,
 * автопереподключение с экспоненциальной задержкой (1…30 с).
 *
 * Поведение один в один с WebSocketTransport.swift (iOS):
 *  - connect() открывает сокет; токен передаёт вызывающий код через auth.token;
 *  - request() suspend-функция: ответ приходит по совпадению id кадра;
 *  - события ("type":"event") публикуются в SharedFlow [events];
 *  - при обрыве соединения незавершённые запросы падают AuraError("disconnected"),
 *    транспорт уходит в reconnecting и поднимается сам.
 */
class WebSocketTransport(private val client: OkHttpClient = defaultClient()) {

    enum class State { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING }

    data class AuraEvent(val name: String, val payload: JsonObject?)

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val counter = AtomicLong(1)
    private val pending = ConcurrentHashMap<String, CompletableDeferred<JsonObject?>>()
    private val scope = CoroutineScope(Dispatchers.IO)

    private val _state = MutableSharedFlow<State>(replay = 1, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    val state: SharedFlow<State> = _state

    private val _events = MutableSharedFlow<AuraEvent>(extraBufferCapacity = 64, onBufferOverflow = BufferOverflow.DROP_OLDEST)
    val events: SharedFlow<AuraEvent> = _events

    @Volatile private var socket: WebSocket? = null
    @Volatile private var url: String = ""
    @Volatile private var intentionallyClosed = false
    @Volatile private var attempt = 0

    fun connect(serverUrl: String) {
        url = serverUrl
        intentionallyClosed = false
        attempt = 0
        openSocket()
    }

    fun disconnect() {
        intentionallyClosed = true
        socket?.close(NORMAL_CLOSURE, "client disconnect")
        socket = null
        _state.tryEmit(State.DISCONNECTED)
    }

    /** Запрос к серверу; возвращает payload кадра "ok" или бросает AuraError. */
    suspend fun request(type: String, payload: JsonObject = JsonObject(emptyMap())): JsonObject? {
        val ws = socket ?: throw AuraError("disconnected", "соединение не установлено")
        val id = "req-${counter.getAndIncrement()}"
        val deferred = CompletableDeferred<JsonObject?>()
        pending[id] = deferred
        val frame = AuraRequestFrame(id = id, type = type, payload = payload)
        val sent = ws.send(json.encodeToString(AuraRequestFrame.serializer(), frame))
        if (!sent) {
            pending.remove(id)
            throw AuraError("disconnected", "не удалось отправить кадр")
        }
        return try {
            deferred.await()
        } finally {
            pending.remove(id)
        }
    }

    // ------------------------------------------------------------------ private

    private fun openSocket() {
        _state.tryEmit(if (attempt == 0) State.CONNECTING else State.RECONNECTING)
        val request = Request.Builder().url(url).build()
        socket = client.newWebSocket(request, Listener())
    }

    private fun scheduleReconnect() {
        if (intentionallyClosed) return
        val delayMs = (1000L shl minOf(attempt, 5)).coerceAtMost(30_000L)
        attempt += 1
        scope.launch {
            delay(delayMs)
            if (!intentionallyClosed) openSocket()
        }
    }

    private fun failPending(cause: AuraError) {
        val snapshot = pending.values.toList()
        pending.clear()
        snapshot.forEach { it.completeExceptionally(cause) }
    }

    private inner class Listener : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            attempt = 0
            _state.tryEmit(State.CONNECTED)
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            val frame = try {
                json.decodeFromString(AuraResponseFrame.serializer(), text)
            } catch (_: Exception) {
                return  // мусорный кадр игнорируем: протокол это допускает
            }
            if (frame.type == "event") {
                val name = frame.event ?: return
                _events.tryEmit(AuraEvent(name, frame.payload))
                return
            }
            val id = frame.id ?: return
            val deferred = pending.remove(id) ?: return
            when (frame.type) {
                "ok" -> deferred.complete(frame.payload)
                else -> deferred.completeExceptionally(
                    AuraError(frame.code ?: "internal_error", frame.message ?: "неизвестная ошибка"),
                )
            }
        }

        override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
            onMessage(webSocket, bytes.utf8())
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            failPending(AuraError("disconnected", t.message ?: "соединение потеряно"))
            scheduleReconnect()
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            failPending(AuraError("disconnected", "соединение закрыто"))
            if (!intentionallyClosed) scheduleReconnect()
        }
    }

    companion object {
        private const val NORMAL_CLOSURE = 1000

        fun defaultClient(): OkHttpClient = OkHttpClient.Builder()
            .pingInterval(java.time.Duration.ofSeconds(25))
            .build()
    }
}
