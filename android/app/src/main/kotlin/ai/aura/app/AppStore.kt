package ai.aura.app

import android.content.Context
import ai.aura.kit.AuraClient
import ai.aura.kit.AuraError
import ai.aura.kit.AuraEventName
import ai.aura.kit.DeepLink
import ai.aura.kit.arr
import ai.aura.kit.bool
import ai.aura.kit.long
import ai.aura.kit.obj
import ai.aura.kit.str
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Центральный стор приложения — аналог AppStore.swift (iOS): WS-соединение,
 * авторизация (включая 2FA и подтверждение email), данные экранов, события
 * сервера, регистрация FCM-устройства. Один экземпляр на процесс.
 *
 * Все сетевые ошибки превращаются в человекочитаемый [errorText] (состояние
 * ошибки есть у каждого экрана — никаких «молча не получилось»).
 */
object AppStore {

    // ------------------------------------------------------------- модели UI

    data class Chat(val id: Long, val title: String, val lastBody: String, val lastAt: String)
    data class Message(val id: Long, val senderId: Long, val body: String, val kind: String, val createdAt: String)
    data class Task(val id: Long, val title: String, val notes: String, val status: String, val remindAt: String)
    data class Confirmation(val id: Long, val tool: String, val summary: String, val status: String)
    data class Notice(val id: Long, val kind: String, val title: String, val body: String, val read: Boolean, val createdAt: String)
    data class PushDevice(val id: Long, val platform: String, val enabled: Boolean)
    data class PermissionRow(val tool: String, val description: String, val dangerous: Boolean, val mode: String)
    data class ConnectionRow(val id: Long, val provider: String, val account: String, val status: String)
    data class TwoFaSetup(val secret: String, val otpauthUri: String)

    sealed class AuthState {
        data object LoggedOut : AuthState()
        data class NeedsEmailCode(val email: String) : AuthState()
        data class Needs2fa(val email: String, val password: String) : AuthState()
        data class LoggedIn(val displayName: String, val email: String) : AuthState()
    }

    enum class Tab { CHATS, TASKS, PENDING, NOTICES, SETTINGS }

    // ------------------------------------------------------------- состояние

    lateinit var store: SecureStore
        private set
    val client = AuraClient()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val _auth = MutableStateFlow<AuthState>(AuthState.LoggedOut)
    val auth: StateFlow<AuthState> = _auth

    private val _errorText = MutableStateFlow<String?>(null)
    val errorText: StateFlow<String?> = _errorText

    private val _askReply = MutableStateFlow("")
    val askReply: StateFlow<String> = _askReply

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy

    private val _tab = MutableStateFlow(Tab.CHATS)
    val tab: StateFlow<Tab> = _tab

    private val _openChatId = MutableStateFlow(0L)
    val openChatId: StateFlow<Long> = _openChatId

    private val _chats = MutableStateFlow<List<Chat>>(emptyList())
    val chats: StateFlow<List<Chat>> = _chats

    private val _messages = MutableStateFlow<List<Message>>(emptyList())
    val messages: StateFlow<List<Message>> = _messages

    private val _tasks = MutableStateFlow<List<Task>>(emptyList())
    val tasks: StateFlow<List<Task>> = _tasks

    private val _confirmations = MutableStateFlow<List<Confirmation>>(emptyList())
    val confirmations: StateFlow<List<Confirmation>> = _confirmations

    private val _notices = MutableStateFlow<List<Notice>>(emptyList())
    val notices: StateFlow<List<Notice>> = _notices

    private val _unread = MutableStateFlow(0L)
    val unread: StateFlow<Long> = _unread

    private val _pushDevices = MutableStateFlow<List<PushDevice>>(emptyList())
    val pushDevices: StateFlow<List<PushDevice>> = _pushDevices

    private val _pushStatus = MutableStateFlow("")
    val pushStatus: StateFlow<String> = _pushStatus

    private val _permissions = MutableStateFlow<List<PermissionRow>>(emptyList())
    val permissions: StateFlow<List<PermissionRow>> = _permissions

    private val _connections = MutableStateFlow<List<ConnectionRow>>(emptyList())
    val connections: StateFlow<List<ConnectionRow>> = _connections

    private val _twoFaEnabled = MutableStateFlow(false)
    val twoFaEnabled: StateFlow<Boolean> = _twoFaEnabled

    private val _twoFaSetup = MutableStateFlow<TwoFaSetup?>(null)
    val twoFaSetup: StateFlow<TwoFaSetup?> = _twoFaSetup

    private val _recoveryCodes = MutableStateFlow<List<String>>(emptyList())
    val recoveryCodes: StateFlow<List<String>> = _recoveryCodes

    /** Запрос голоса/deep link, который надо показать после готовности UI. */
    private val _pendingAsk = MutableStateFlow<String?>(null)
    val pendingAsk: StateFlow<String?> = _pendingAsk

    private val _wantVoice = MutableStateFlow(false)
    val wantVoice: StateFlow<Boolean> = _wantVoice

    private var eventsStarted = false

    // ------------------------------------------------------------- запуск

    fun init(context: Context) {
        if (this::store.isInitialized) return
        store = SecureStore(context.applicationContext)
        startEvents()
        if (!store.accessToken.isNullOrEmpty()) {
            connectAndAuthenticate()
        }
    }

    private fun startEvents() {
        if (eventsStarted) return
        eventsStarted = true
        scope.launch {
            client.events.collect { event ->
                when (event.name) {
                    AuraEventName.CHAT_MESSAGE -> refreshChats()
                    AuraEventName.TASK_DUE -> refreshTasks()
                    AuraEventName.NOTIFICATION_NEW -> refreshNotifications()
                }
            }
        }
    }

    private fun connectAndAuthenticate() {
        scope.launch {
            guard {
                client.connect(store.serverUrl)
                waitForConnected()
                val token = store.accessToken ?: throw AuraError("unauthorized", "нет токена")
                try {
                    client.authenticate(token)
                } catch (error: AuraError) {
                    // Access-токен истёк/отозван — пробуем ротацию refresh.
                    val refresh = store.refreshToken
                        ?: throw AuraError("unauthorized", "сессия истекла, войдите снова")
                    val rotated = client.refresh(refresh, store.deviceName)
                    store.accessToken = rotated.str("token")
                    store.refreshToken = rotated.str("refresh_token")
                    client.authenticate(rotated.str("token"))
                }
                val me = client.me()
                val user = me.obj("user")
                _auth.value = AuthState.LoggedIn(user.str("display_name"), user.str("email"))
                afterLogin()
            }
        }
    }

    private suspend fun waitForConnected() {
        // Первое состояние транспорт публикует сразу (replay=1); ждём CONNECTED
        // не дольше 10 секунд — иначе ошибка соединения, а не вечное ожидание.
        kotlinx.coroutines.withTimeoutOrNull(10_000) {
            client.state.first { it == ai.aura.kit.WebSocketTransport.State.CONNECTED }
        } ?: throw AuraError("disconnected", "сервер недоступен: ${store.serverUrl}")
    }

    // ------------------------------------------------------------- авторизация

    fun login(email: String, password: String) = scope.launch {
        guard {
            client.connect(store.serverUrl)
            waitForConnected()
            try {
                applyAuth(client.login(email, password, store.deviceId, store.deviceName))
            } catch (error: AuraError) {
                when (error.code) {
                    "requires_2fa" -> _auth.value = AuthState.Needs2fa(email, password)
                    "email_not_verified" -> _auth.value = AuthState.NeedsEmailCode(email)
                    else -> throw error
                }
            }
        }
    }

    fun register(displayName: String, email: String, password: String) = scope.launch {
        guard {
            client.connect(store.serverUrl)
            waitForConnected()
            client.register(email, password, displayName)
            _auth.value = AuthState.NeedsEmailCode(email)
            _errorText.value = "Мы отправили код подтверждения на $email"
        }
    }

    fun verifyEmail(email: String, code: String) = scope.launch {
        guard {
            client.verifyEmail(email, code)
            _auth.value = AuthState.LoggedOut
            _errorText.value = "Email подтверждён — теперь можно войти"
        }
    }

    fun resendCode(email: String) = scope.launch {
        guard { client.resendCode(email) }
    }

    fun login2fa(code: String, trustDevice: Boolean) = scope.launch {
        val state = _auth.value as? AuthState.Needs2fa ?: return@launch
        guard {
            applyAuth(client.login2fa(state.email, state.password, code, trustDevice,
                store.deviceId, store.deviceName))
        }
    }

    fun logout() = scope.launch {
        guard {
            runCatching { client.logout() }
            store.clearTokens()
            client.disconnect()
            _auth.value = AuthState.LoggedOut
            resetData()
        }
    }

    fun setServerUrl(url: String) {
        if (!url.startsWith("ws://") && !url.startsWith("wss://")) {
            _errorText.value = "Адрес сервера начинается с ws:// или wss://"
            return
        }
        store.serverUrl = url
        if (_auth.value is AuthState.LoggedIn) logout()
        client.disconnect()
        _errorText.value = null
    }

    private suspend fun applyAuth(payload: JsonObject?) {
        store.accessToken = payload.str("token")
        store.refreshToken = payload.str("refresh_token")
        val user = payload.obj("user")
        _auth.value = AuthState.LoggedIn(user.str("display_name"), user.str("email"))
        afterLogin()
    }

    private suspend fun afterLogin() {
        refreshChats()
        refreshTasks()
        refreshConfirmations()
        refreshNotifications()
        refreshPermissions()
        refreshIntegrations()
        refresh2faStatus()
        registerPushDevice()
    }

    private fun resetData() {
        _chats.value = emptyList(); _messages.value = emptyList()
        _tasks.value = emptyList(); _confirmations.value = emptyList()
        _notices.value = emptyList(); _unread.value = 0
        _pushDevices.value = emptyList(); _permissions.value = emptyList()
        _connections.value = emptyList(); _askReply.value = ""
    }

    // ------------------------------------------------------------- Аура/голос

    fun ask(text: String) = scope.launch {
        if (text.isBlank()) return@launch
        guard {
            val result = client.agentAsk(text, _openChatId.value)
            _askReply.value = result.str("reply").ifEmpty { "Аура ничего не ответила" }
            refreshChats(); refreshTasks(); refreshConfirmations(); refreshNotifications()
        }
    }

    fun consumePendingAsk() { _pendingAsk.value = null }
    fun consumeWantVoice() { _wantVoice.value = false }
    fun requestVoice() { _wantVoice.value = true }
    fun selectTab(value: Tab) { _tab.value = value }

    // ------------------------------------------------------------- чаты

    fun refreshChats() = scope.launch {
        guard {
            val payload = client.chatList()
            _chats.value = payload.arr("chats").mapNotNull { element ->
                val chat = element as? JsonObject ?: return@mapNotNull null
                Chat(chat.long("id"), chat.str("title").ifEmpty { "Чат ${chat.long("id")}" },
                    chat.str("last_message_body"), chat.str("last_message_at"))
            }
        }
    }

    fun openChat(id: Long) = scope.launch {
        _openChatId.value = id
        _tab.value = Tab.CHATS
        loadHistory()
    }

    fun openChatByContact(contact: String) = scope.launch {
        guard {
            val payload = client.chatOpen(contact)
            val chat = payload.obj("chat")
            _openChatId.value = chat.long("id")
            _tab.value = Tab.CHATS
            loadHistory()
        }
    }

    private suspend fun loadHistory() {
        val payload = client.chatHistory(_openChatId.value)
        _messages.value = payload.arr("messages").mapNotNull { element ->
            val message = element as? JsonObject ?: return@mapNotNull null
            Message(message.long("id"), message.long("sender_id"), message.str("body"),
                message.str("kind").ifEmpty { "text" }, message.str("created_at"))
        }
        runCatching { client.request("chat.read", buildJsonObject { put("chat_id", _openChatId.value) }) }
    }

    fun sendMessage(text: String) = scope.launch {
        if (text.isBlank() || _openChatId.value == 0L) return@launch
        guard {
            client.chatSend(_openChatId.value, text)
            loadHistory()
        }
    }

    // ------------------------------------------------------------- задачи

    fun refreshTasks() = scope.launch {
        guard {
            val payload = client.tasksList()
            _tasks.value = payload.arr("tasks").mapNotNull { element ->
                val task = element as? JsonObject ?: return@mapNotNull null
                Task(task.long("id"), task.str("title"), task.str("notes"),
                    task.str("status"), task.str("remind_at"))
            }
        }
    }

    fun createTask(title: String, remindAt: String) = scope.launch {
        if (title.isBlank()) return@launch
        guard { client.taskCreate(title, remindAt = remindAt); refreshTasks() }
    }

    fun setTaskStatus(id: Long, status: String) = scope.launch {
        guard { client.taskSetStatus(id, status); refreshTasks() }
    }

    fun deleteTask(id: Long) = scope.launch {
        guard { client.taskDelete(id); refreshTasks() }
    }

    // ------------------------------------------- подтверждения и разрешения

    fun refreshConfirmations() = scope.launch {
        guard {
            val payload = client.confirmationsList()
            _confirmations.value = payload.arr("actions").mapNotNull { element ->
                val action = element as? JsonObject ?: return@mapNotNull null
                Confirmation(action.long("id"), action.str("tool"), action.str("summary"),
                    action.str("status"))
            }
        }
    }

    fun decide(id: Long, approve: Boolean) = scope.launch {
        guard {
            client.confirmationDecide(id, approve)
            refreshConfirmations(); refreshChats(); refreshNotifications()
        }
    }

    fun refreshPermissions() = scope.launch {
        guard {
            val payload = client.permissionsList()
            _permissions.value = payload.arr("tools").mapNotNull { element ->
                val tool = element as? JsonObject ?: return@mapNotNull null
                PermissionRow(tool.str("tool"), tool.str("description"),
                    tool.bool("dangerous"), tool.str("mode"))
            }
        }
    }

    fun setPermission(tool: String, mode: String) = scope.launch {
        guard { client.permissionSet(tool, mode); refreshPermissions() }
    }

    // ------------------------------------------------------------- уведомления

    fun refreshNotifications() = scope.launch {
        guard {
            val payload = client.notificationsList()
            _notices.value = payload.arr("notifications").mapNotNull { element ->
                val notice = element as? JsonObject ?: return@mapNotNull null
                Notice(notice.long("id"), notice.str("kind"), notice.str("title"),
                    notice.str("body"), notice.bool("read"), notice.str("created_at"))
            }
            _unread.value = payload.long("unread")
            refreshPushDevices()
        }
    }

    fun markAllRead() = scope.launch {
        guard { client.notificationsRead(); refreshNotifications() }
    }

    fun markRead(id: Long) = scope.launch {
        guard { client.notificationsRead(id); refreshNotifications() }
    }

    fun refreshPushDevices() = scope.launch {
        guard {
            val payload = client.devicesPushList()
            _pushDevices.value = payload.arr("devices").mapNotNull { element ->
                val device = element as? JsonObject ?: return@mapNotNull null
                PushDevice(device.long("id"), device.str("platform"), device.bool("enabled"))
            }
        }
    }

    fun revokePushDevice(id: Long) = scope.launch {
        guard { client.devicesPushRevoke(id); refreshPushDevices() }
    }

    /** Ротация FCM-токена (FcmService.onNewToken): upsert на сервере. */
    fun reRegisterPushToken(token: String) {
        scope.launch {
            if (_auth.value !is AuthState.LoggedIn) return@launch
            runCatching { client.devicesPushRegister("fcm", token) }
                .onSuccess { _pushStatus.value = "Push включён (FCM)" }
        }
    }

    /** Синхронный (suspend) вариант обновления — для FcmService. */
    suspend fun refreshNotificationsBlocking() {
        if (_auth.value !is AuthState.LoggedIn) return
        val payload = client.notificationsList()
        _notices.value = payload.arr("notifications").mapNotNull { element ->
            val notice = element as? JsonObject ?: return@mapNotNull null
            Notice(notice.long("id"), notice.str("kind"), notice.str("title"),
                notice.str("body"), notice.bool("read"), notice.str("created_at"))
        }
        _unread.value = payload.long("unread")
    }

    // ------------------------------------------------------------- 2FA

    fun refresh2faStatus() = scope.launch {
        guard { _twoFaEnabled.value = client.status2fa().bool("enabled") }
    }

    fun setup2fa() = scope.launch {
        guard {
            val payload = client.setup2fa()
            _twoFaSetup.value = TwoFaSetup(payload.str("secret"), payload.str("otpauth_uri"))
        }
    }

    fun confirm2fa(code: String) = scope.launch {
        guard {
            val payload = client.confirm2fa(code)
            _recoveryCodes.value = payload.arr("recovery_codes").map { it.toString().trim('"') }
            _twoFaSetup.value = null
            _twoFaEnabled.value = true
        }
    }

    fun disable2fa(password: String) = scope.launch {
        guard {
            client.disable2fa(password)
            _twoFaEnabled.value = false
            _recoveryCodes.value = emptyList()
        }
    }

    // ------------------------------------------------------------- интеграции

    fun refreshIntegrations() = scope.launch {
        guard {
            val payload = client.integrationsList()
            _connections.value = payload.arr("connections").mapNotNull { element ->
                val row = element as? JsonObject ?: return@mapNotNull null
                ConnectionRow(row.long("id"), row.str("provider"), row.str("account"),
                    row.str("status"))
            }
        }
    }

    /** Возвращает authorize_url для Custom Tabs или текст ошибки. */
    suspend fun beginIntegration(provider: String): String {
        return try {
            val payload = client.integrationBegin(provider, "aura://oauth")
            payload.str("authorize_url")
        } catch (error: AuraError) {
            _errorText.value = describe(error)
            ""
        }
    }

    fun finishIntegration(provider: String, code: String, state: String) = scope.launch {
        guard { client.integrationCallback(provider, code, state); refreshIntegrations() }
    }

    fun revokeIntegration(id: Long) = scope.launch {
        guard { client.integrationRevoke(id); refreshIntegrations() }
    }

    fun syncIntegration(id: Long) = scope.launch {
        guard { client.integrationSync(id); refreshIntegrations() }
    }

    // ------------------------------------------------------------- push (FCM)

    /**
     * Регистрация FCM-токена на сервере (devices.push.register, platform "fcm").
     * Без google-services.json FirebaseApp не инициализирован — честно пишем
     * статус «не настроен» и не делаем вид, что push работает.
     */
    fun registerPushDevice() {
        if (_auth.value !is AuthState.LoggedIn) return
        val context = contextRef ?: return
        if (FirebaseApp.getApps(context).isEmpty()) {
            _pushStatus.value = "Push не настроен: нет app/google-services.json (см. README)"
            return
        }
        scope.launch {
            try {
                val token = awaitTask(FirebaseMessaging.getInstance().token)
                client.devicesPushRegister("fcm", token)
                _pushStatus.value = "Push включён (FCM)"
                refreshPushDevices()
            } catch (error: Exception) {
                _pushStatus.value = "FCM недоступен: ${error.message}"
            }
        }
    }

    @Volatile var contextRef: Context? = null
        private set

    fun bindContext(context: Context) { contextRef = context.applicationContext }

    // ------------------------------------------------------------- deep links

    fun handleDeepLink(url: String) {
        when (val link = DeepLink.parse(url)) {
            is DeepLink.Chats -> if (link.id != null) openChat(link.id) else _tab.value = Tab.CHATS
            DeepLink.Voice -> _wantVoice.value = true
            is DeepLink.Ask -> if (link.text.isBlank()) _wantVoice.value = true else ask(link.text)
            DeepLink.Tasks -> _tab.value = Tab.TASKS
            DeepLink.Notifications -> _tab.value = Tab.NOTICES
            DeepLink.Settings -> _tab.value = Tab.SETTINGS
            is DeepLink.OAuth -> finishIntegration(link.provider, link.code, link.state)
            null -> Unit  // неизвестная ссылка игнорируется без краша
        }
    }

    // ------------------------------------------------------------- служебное

    private suspend fun guard(block: suspend () -> Unit) {
        _busy.value = true
        _errorText.value = null
        try {
            block()
        } catch (error: AuraError) {
            _errorText.value = describe(error)
        } catch (error: Exception) {
            _errorText.value = error.message ?: "неизвестная ошибка"
        } finally {
            _busy.value = false
        }
    }

    private fun describe(error: AuraError): String = when (error.code) {
        "unauthorized" -> "Неверный email или пароль"
        "forbidden" -> error.message
        "requires_2fa" -> "Нужен код двухфакторной аутентификации"
        "email_not_verified" -> "Email не подтверждён"
        "conflict" -> "Такой email уже зарегистрирован"
        "not_found" -> "Не найдено"
        "disconnected" -> "Нет соединения с сервером"
        "upstream_error" -> "AI-сервис недоступен, попробуйте позже"
        else -> error.message
    }

    fun clearError() { _errorText.value = null }

    /** Task<T> → suspend без лишней зависимости (play-services не нужен). */
    private suspend fun <T> awaitTask(task: com.google.android.gms.tasks.Task<T>): T =
        suspendCancellableCoroutine { continuation ->
            task.addOnSuccessListener { value -> continuation.resumeWith(Result.success(value)) }
            task.addOnCanceledListener {
                continuation.resumeWith(
                    Result.failure(AuraError("canceled", "операция отменена")),
                )
            }
            task.addOnFailureListener { error -> continuation.resumeWith(Result.failure(error)) }
        }
}
