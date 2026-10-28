package app.selfvoy

import android.content.Context
import app.selfvoy.kit.SelfvoyClient
import app.selfvoy.kit.SelfvoyError
import app.selfvoy.kit.SelfvoyEventName
import app.selfvoy.kit.SelfvoyUser
import app.selfvoy.kit.DeepLink
import app.selfvoy.kit.Privacy
import app.selfvoy.kit.Booking
import app.selfvoy.kit.BookingPage
import app.selfvoy.kit.BookingPageFields
import app.selfvoy.kit.CalendarFeed
import app.selfvoy.kit.PackageIssueRequest
import app.selfvoy.kit.PackageIssued
import app.selfvoy.kit.PackagePurchase
import app.selfvoy.kit.PaymentsStatus
import app.selfvoy.kit.Plan
import app.selfvoy.kit.SelfvoyErrorCode
import app.selfvoy.kit.SelfvoyNoticeKind
import app.selfvoy.kit.SlotsPost
import app.selfvoy.kit.TelegramLink
import app.selfvoy.kit.TelegramStatus
import app.selfvoy.kit.YooKassaCredentials
import app.selfvoy.kit.telegramNotificationsEnabled
import app.selfvoy.ui.theme.SelfvoyPalette
import app.selfvoy.ui.theme.ThemeMode
import app.selfvoy.kit.arr
import app.selfvoy.kit.bool
import app.selfvoy.kit.long
import app.selfvoy.kit.obj
import app.selfvoy.kit.str
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonPrimitive
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

    data class Member(val userId: Long, val name: String, val email: String)
    data class Chat(val id: Long, val title: String, val lastBody: String, val lastAt: String, val members: List<Member>)
    data class Message(val id: Long, val senderId: Long, val body: String, val kind: String, val createdAt: String,
                       val senderName: String = "")
    data class Task(val id: Long, val title: String, val notes: String, val status: String, val remindAt: String,
                    val dueAt: String = "", val completedAt: String = "") {
        /** Когда задача: срок, иначе время напоминания. */
        val whenAt: String get() = dueAt.ifEmpty { remindAt }
    }
    /** Собеседник: человек из чатов (кроме меня). */
    data class Contact(val userId: Long, val name: String, val email: String, val chatId: Long,
                       val lastBody: String, val lastAt: String)
    data class FoundUser(val id: Long, val name: String, val online: Boolean)
    /** Действие Selfvoy, ждущее решения, прямо под его репликой. */
    data class DialogAction(val confirmationId: Long, val summary: String, val tool: String)
    data class DialogEntry(val id: Long, val fromSelfvoy: Boolean, val text: String, val error: Boolean = false,
                           val actions: List<DialogAction> = emptyList(), val hasA2a: Boolean = false)
    data class Slot(val start: String, val end: String)
    data class Place(val name: String, val city: String, val tags: List<String>, val rating: String)
    /** Один раунд agent.negotiate: запрос рядом с ответом сервера. */
    data class Negotiation(
        val target: String, val topic: String, val durationMinutes: Int, val windowHours: Int,
        val round: Int, val at: Long,
        val accepted: Boolean, val slot: Slot?, val place: Place?, val counterSlots: List<Slot>,
        val message: String, val booking: String? = null,
    )
    data class MemoryEntry(val kind: String, val text: String)
    data class Confirmation(val id: Long, val tool: String, val summary: String, val status: String)
    data class Notice(val id: Long, val kind: String, val title: String, val body: String, val read: Boolean, val createdAt: String)
    data class PushDevice(val id: Long, val platform: String, val enabled: Boolean)
    data class PermissionRow(val tool: String, val description: String, val dangerous: Boolean, val mode: String)
    data class ConnectionRow(val id: Long, val provider: String, val account: String, val status: String)
    data class TwoFaSetup(val secret: String, val otpauthUri: String)
    /** Свободное окно страницы записи для ленты дня («Предложить»). */
    data class FreeSlot(val start: String, val end: String, val pageId: Long)

    /**
     * Итог операции экрана: значение или текст ошибки для показа рядом с кнопкой
     * ([proRequired] — нужен Pro: показать, как его получить).
     */
    data class Outcome<T>(val value: T? = null, val error: String? = null, val proRequired: Boolean = false) {
        val ok: Boolean get() = error == null
    }

    /** Действие из «+» или с «Сегодня», которое выполнит открывшийся экран. */
    enum class PendingAction { NEW_PAGE, SLOTS_POST, ISSUE_PACKAGE }

    sealed class AuthState {
        data object LoggedOut : AuthState()
        data class NeedsEmailCode(val email: String) : AuthState()
        data class Needs2fa(val email: String, val password: String) : AuthState()
        data class NeedsResetCode(val email: String) : AuthState()
        data class LoggedIn(val displayName: String, val email: String, val bio: String = "") : AuthState()
    }

    /**
     * Разделы 2026: четыре во вкладках снизу («Сегодня · Запись · ИИ · Ещё»),
     * остальные открываются из «Ещё» и шапки «Сегодня» и закрываются «назад».
     */
    enum class Tab {
        HOME, BOOKING, SELFVOY, MORE,
        NEGOTIATIONS, TASKS, CONTACTS, SETTINGS, CONFIRMATIONS, NOTICES, PACKAGES, INTEGRATIONS;

        /** Вкладка панели, к которой относится раздел (подсветка и «назад»). */
        val root: Tab get() = when (this) {
            HOME, BOOKING, SELFVOY, MORE -> this
            else -> MORE
        }
    }

    // ------------------------------------------------------------- состояние

    lateinit var store: SecureStore
        private set
    val client = SelfvoyClient()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val _auth = MutableStateFlow<AuthState>(AuthState.LoggedOut)
    val auth: StateFlow<AuthState> = _auth

    private val _errorText = MutableStateFlow<String?>(null)
    val errorText: StateFlow<String?> = _errorText

    private val _askReply = MutableStateFlow("")
    val askReply: StateFlow<String> = _askReply

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy

    private val _tab = MutableStateFlow(Tab.HOME)
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

    // Страницы записи (booking.*) и рефералы (referral.*).
    private val _bookingPages = MutableStateFlow<List<app.selfvoy.kit.BookingPage>?>(null)
    val bookingPages: StateFlow<List<app.selfvoy.kit.BookingPage>?> = _bookingPages
    private val _bookings = MutableStateFlow<List<app.selfvoy.kit.Booking>>(emptyList())
    val bookings: StateFlow<List<app.selfvoy.kit.Booking>> = _bookings
    private val _referral = MutableStateFlow<app.selfvoy.kit.ReferralInfo?>(null)
    val referral: StateFlow<app.selfvoy.kit.ReferralInfo?> = _referral

    /** Тариф пользователя (user.plan); null — старый сервер. */
    private val _plan = MutableStateFlow<Plan?>(null)
    val plan: StateFlow<Plan?> = _plan

    /** Свободные окна первой открытой страницы (те же, что видит гость). */
    private val _freeSlots = MutableStateFlow<List<FreeSlot>>(emptyList())
    val freeSlots: StateFlow<List<FreeSlot>> = _freeSlots

    /** Пакеты и абонементы (package.list); null — ещё не загружены. */
    private val _packages = MutableStateFlow<List<PackagePurchase>?>(null)
    val packages: StateFlow<List<PackagePurchase>?> = _packages

    private val _telegram = MutableStateFlow<TelegramStatus?>(null)
    val telegram: StateFlow<TelegramStatus?> = _telegram

    private val _payments = MutableStateFlow<PaymentsStatus?>(null)
    val payments: StateFlow<PaymentsStatus?> = _payments

    private val _calendarFeed = MutableStateFlow<CalendarFeed?>(null)
    val calendarFeed: StateFlow<CalendarFeed?> = _calendarFeed

    private val _pendingAction = MutableStateFlow<PendingAction?>(null)
    val pendingAction: StateFlow<PendingAction?> = _pendingAction

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

    /**
     * Deep link selfvoy://signup?ref=…: null — ссылки не было; иначе код приглашения
     * ("" — без валидного кода). Экран входа открывает регистрацию и забирает код.
     */
    private val _signupRef = MutableStateFlow<String?>(null)
    val signupRef: StateFlow<String?> = _signupRef

    /** Онбординг-опрос: показываем после входа, пока prefs.onboarded != true. */
    private val _needOnboarding = MutableStateFlow(false)
    val needOnboarding: StateFlow<Boolean> = _needOnboarding

    private val _userId = MutableStateFlow(0L)
    val userId: StateFlow<Long> = _userId

    private val _contacts = MutableStateFlow<List<Contact>>(emptyList())
    val contacts: StateFlow<List<Contact>> = _contacts

    private val _dialog = MutableStateFlow<List<DialogEntry>>(emptyList())
    val dialog: StateFlow<List<DialogEntry>> = _dialog

    private val _negotiation = MutableStateFlow<Negotiation?>(null)
    val negotiation: StateFlow<Negotiation?> = _negotiation

    private val _negotiationHistory = MutableStateFlow<List<Negotiation>>(emptyList())
    val negotiationHistory: StateFlow<List<Negotiation>> = _negotiationHistory

    private val _userResults = MutableStateFlow<List<FoundUser>>(emptyList())
    val userResults: StateFlow<List<FoundUser>> = _userResults

    private val _memory = MutableStateFlow<List<MemoryEntry>>(emptyList())
    val memory: StateFlow<List<MemoryEntry>> = _memory

    private val _prefs = MutableStateFlow<JsonObject?>(null)
    val prefs: StateFlow<JsonObject?> = _prefs

    /** Приватность в переговорах (prefs.privacy); null — ещё не загружена. */
    private val _privacy = MutableStateFlow<Privacy?>(null)
    val privacy: StateFlow<Privacy?> = _privacy

    /** Ошибка последнего переключения приватности (текст сервера). */
    private val _privacyError = MutableStateFlow<String?>(null)
    val privacyError: StateFlow<String?> = _privacyError

    private val _profileSaving = MutableStateFlow(false)
    val profileSaving: StateFlow<Boolean> = _profileSaving

    /** Ошибка profile.update (текст сервера) — показывается в карточке профиля. */
    private val _profileError = MutableStateFlow<String?>(null)
    val profileError: StateFlow<String?> = _profileError

    /** Ответ «С чем помогать?» из регистрации — уходит в память после первого входа. */
    private var pendingFocus = ""
    private var dialogSeq = 0L
    private var eventsStarted = false

    // ------------------------------------------------------------- запуск

    fun init(context: Context) {
        if (this::store.isInitialized) return
        store = SecureStore(context.applicationContext)
        SelfvoyPalette.mode = ThemeMode.of(store.themeMode)
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
                    SelfvoyEventName.CHAT_MESSAGE -> refreshChats()
                    SelfvoyEventName.TASK_DUE -> refreshTasks()
                    SelfvoyEventName.NOTIFICATION_NEW -> {
                        refreshNotifications()
                        val kind = event.payload.str("kind")
                        // «Сегодня» показывает записи всегда — перечитываем при любом событии записи.
                        if (SelfvoyNoticeKind.touchesBookings(kind)) refreshBooking()
                        if (SelfvoyNoticeKind.touchesPackages(kind) && _packages.value != null) refreshPackages()
                        if (SelfvoyNoticeKind.touchesReferral(kind)) {
                            if (_referral.value != null) refreshReferral()
                            // Месяц Pro начислен — тариф в user.plan изменился.
                            if (kind == SelfvoyNoticeKind.REFERRAL_REWARD) refreshMe()
                        }
                    }
                    SelfvoyEventName.SESSION_REVOKED -> onSessionRevoked(event.payload.str("reason"))
                }
            }
        }
    }

    private fun connectAndAuthenticate() {
        scope.launch {
            guard {
                client.connect(store.serverUrl)
                waitForConnected()
                val token = store.accessToken ?: throw SelfvoyError("unauthorized", "нет токена")
                try {
                    client.authenticate(token)
                } catch (error: SelfvoyError) {
                    // Access-токен истёк/отозван — пробуем ротацию refresh.
                    val refresh = store.refreshToken
                        ?: throw SelfvoyError("unauthorized", "сессия истекла, войдите снова")
                    val rotated = client.refresh(refresh, store.deviceName)
                    store.accessToken = rotated.str("token")
                    store.refreshToken = rotated.str("refresh_token")
                    client.authenticate(rotated.str("token"))
                }
                // auth.me возвращает пользователя прямо в payload (без обёртки user).
                val me = client.me()
                applyUser(SelfvoyUser.from(me.obj("user") ?: me))
                afterLogin()
            }
        }
    }

    private suspend fun waitForConnected() {
        // Первое состояние транспорт публикует сразу (replay=1); ждём CONNECTED
        // не дольше 10 секунд — иначе ошибка соединения, а не вечное ожидание.
        kotlinx.coroutines.withTimeoutOrNull(10_000) {
            client.state.first { it == app.selfvoy.kit.WebSocketTransport.State.CONNECTED }
        } ?: throw SelfvoyError("disconnected", "сервер недоступен: ${store.serverUrl}")
    }

    // ------------------------------------------------------------- авторизация

    fun login(email: String, password: String) = scope.launch {
        guard {
            client.connect(store.serverUrl)
            waitForConnected()
            try {
                applyAuth(client.login(email, password, store.deviceId, store.deviceName))
            } catch (error: SelfvoyError) {
                when (error.code) {
                    "requires_2fa" -> _auth.value = AuthState.Needs2fa(email, password)
                    "email_not_verified" -> _auth.value = AuthState.NeedsEmailCode(email)
                    else -> throw error
                }
            }
        }
    }

    /** [ref] — код приглашения (необязательно); неверный код клиент не отправляет. */
    fun register(displayName: String, email: String, password: String, focus: String = "", ref: String = "") = scope.launch {
        pendingFocus = focus.trim()
        guard {
            client.connect(store.serverUrl)
            waitForConnected()
            client.register(email, password, displayName, ref)
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

    fun forgotPassword(email: String) = scope.launch {
        if (email.isBlank()) { _errorText.value = "Введите email"; return@launch }
        guard {
            client.connect(store.serverUrl)
            waitForConnected()
            client.forgotPassword(email.trim())
            _auth.value = AuthState.NeedsResetCode(email.trim())
            _errorText.value = "Если такой аккаунт есть, код отправлен на ${email.trim()}"
        }
    }

    fun resetPassword(email: String, code: String, newPassword: String) = scope.launch {
        guard {
            client.resetPassword(email, code.trim(), newPassword)
            _auth.value = AuthState.LoggedOut
            _errorText.value = "Пароль изменён — войдите с новым паролем"
        }
    }

    fun cancelAuthFlow() { _auth.value = AuthState.LoggedOut }

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

    /**
     * Сервер отозвал сессию этого соединения и сейчас закроет сокет (код 4401):
     * токены мертвы — не переподключаемся с ними, а сразу возвращаемся на вход.
     */
    private fun onSessionRevoked(reason: String) {
        store.clearTokens()
        client.disconnect()
        _auth.value = AuthState.LoggedOut
        resetData()
        _errorText.value = when (reason) {
            "password_changed", "password_reset" -> "Пароль изменён — войдите снова"
            "account_disabled" -> "Аккаунт заблокирован администратором"
            "logout" -> "Вы вышли из аккаунта на другом устройстве"
            else -> "Сессия завершена — войдите снова"
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
        applyUser(SelfvoyUser.from(payload.obj("user")))
        afterLogin()
    }

    private fun applyUser(user: SelfvoyUser) {
        _userId.value = user.id
        _plan.value = user.plan
        _auth.value = AuthState.LoggedIn(user.displayName, user.email, user.bio)
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
        checkOnboarding()
        refreshMemory()
        refreshBooking()
        // Deep link selfvoy://settings пришёл до входа: вкладка уже выбрана, догружаем то, чего нет выше.
        if (_tab.value == Tab.SETTINGS) refreshReferral()
        if (pendingFocus.isNotEmpty()) {
            val focus = pendingFocus
            pendingFocus = ""
            runCatching { client.memoryAdd("Просит помогать с: $focus", "preference") }
        }
    }

    /** prefs.onboarded ставит сервер, когда сохранены поля анкеты. */
    private suspend fun checkOnboarding() {
        runCatching {
            val prefs = client.prefsGet()
            _prefs.value = prefs
            _privacy.value = Privacy.fromPrefs(prefs)
            _needOnboarding.value = !prefs.bool("onboarded")
        }
    }

    // ------------------------------------------------------------- профиль

    /**
     * profile.update: уходят только изменённые поля (null — не трогать).
     * Успех — обновляем пользователя в сторе; ошибка — текст сервера в [profileError].
     */
    fun saveProfile(displayName: String?, bio: String?) = scope.launch {
        if (displayName == null && bio == null) return@launch
        _profileSaving.value = true
        _profileError.value = null
        try {
            applyUser(client.profileUpdate(displayName = displayName, bio = bio))
            _errorText.value = "Профиль сохранён"
        } catch (error: SelfvoyError) {
            _profileError.value = if (error.code == "bad_request") error.serverMessage else describe(error)
        } catch (error: Exception) {
            _profileError.value = error.message ?: "неизвестная ошибка"
        } finally {
            _profileSaving.value = false
        }
    }

    fun clearProfileError() { _profileError.value = null }

    // ------------------------------------------------------------- приватность

    fun refreshPrivacy() = scope.launch {
        runCatching { client.privacyGet() }.onSuccess { _privacy.value = it }
    }

    /**
     * Переключатель сохраняется сразу (prefs.set {"privacy":{flag}}). Показываем новое
     * значение оптимистично; при ошибке возвращаем флаг назад и пишем текст ошибки.
     */
    fun setPrivacyFlag(flag: String, value: Boolean) = scope.launch {
        val current = _privacy.value ?: return@launch
        _privacyError.value = null
        _privacy.value = current.with(flag, value)
        try {
            _privacy.value = when (flag) {
                Privacy.SHARE_FREE_SLOTS_ONLY -> client.privacySet(shareFreeSlotsOnly = value)
                Privacy.HIDE_EVENT_TITLES -> client.privacySet(hideEventTitles = value)
                Privacy.ALLOW_PLACE_CHOICE -> client.privacySet(allowPlaceChoice = value)
                else -> return@launch
            }
        } catch (error: Exception) {
            _privacy.value = (_privacy.value ?: current).with(flag, !value)
            _privacyError.value = when (error) {
                is SelfvoyError -> if (error.code == "bad_request") error.serverMessage else describe(error)
                else -> error.message ?: "неизвестная ошибка"
            }
        }
    }

    private fun Privacy.with(flag: String, value: Boolean): Privacy = when (flag) {
        Privacy.SHARE_FREE_SLOTS_ONLY -> copy(shareFreeSlotsOnly = value)
        Privacy.HIDE_EVENT_TITLES -> copy(hideEventTitles = value)
        Privacy.ALLOW_PLACE_CHOICE -> copy(allowPlaceChoice = value)
        else -> this
    }

    /** Сохранить анкету (день рождения, аллергии, диета, …) через prefs.set. */
    fun submitOnboarding(
        birthday: String,
        allergies: List<String>,
        diet: List<String>,
        transport: String,
        city: String,
        budget: String,
    ) = scope.launch {
        guard {
            val payload = buildJsonObject {
                if (birthday.isNotEmpty()) put("birthday", birthday)
                if (allergies.isNotEmpty()) put("allergies", kotlinx.serialization.json.JsonArray(
                    allergies.map { kotlinx.serialization.json.JsonPrimitive(it) }))
                if (diet.isNotEmpty()) put("diet", kotlinx.serialization.json.JsonArray(
                    diet.map { kotlinx.serialization.json.JsonPrimitive(it) }))
                put("transport", transport)
                if (city.isNotEmpty()) put("city", city)
                budget.toDoubleOrNull()?.let { put("budget_limit", it) }
            }
            client.prefsSet(payload)
            _needOnboarding.value = false
        }
    }

    /** «Пропустить»: не сохраняем пустую анкету, но больше не показываем. */
    fun skipOnboarding() { _needOnboarding.value = false }

    private fun resetData() {
        _chats.value = emptyList(); _messages.value = emptyList()
        _tasks.value = emptyList(); _confirmations.value = emptyList()
        _notices.value = emptyList(); _unread.value = 0
        _pushDevices.value = emptyList(); _permissions.value = emptyList()
        _bookingPages.value = null; _bookings.value = emptyList(); _referral.value = null
        _connections.value = emptyList(); _askReply.value = ""
        _contacts.value = emptyList(); _dialog.value = emptyList(); _negotiation.value = null
        _negotiationHistory.value = emptyList(); _userResults.value = emptyList(); _memory.value = emptyList()
        _userId.value = 0; _tab.value = Tab.HOME
        _prefs.value = null; _privacy.value = null; _privacyError.value = null; _profileError.value = null
        _plan.value = null; _freeSlots.value = emptyList(); _packages.value = null; _telegram.value = null
        _payments.value = null; _calendarFeed.value = null; _pendingAction.value = null
    }

    /** Перечитать пользователя (тариф после награды за приглашение). */
    fun refreshMe() = scope.launch {
        runCatching {
            val me = client.me()
            applyUser(SelfvoyUser.from(me.obj("user") ?: me))
        }
    }

    fun setThemeMode(mode: ThemeMode) {
        SelfvoyPalette.mode = mode
        store.themeMode = mode.key
    }

    // ------------------------------------------------------------- Selfvoy/голос

    /**
     * Запрос к Selfvoy («Мой Selfvoy», Home, голос, шорткаты). Реплика и ответ ложатся в
     * диалог; опасные действия из results[] — кнопками «Подтвердить / Отклонить».
     */
    fun ask(text: String, chatId: Long = 0) = scope.launch {
        if (text.isBlank()) return@launch
        if (chatId == 0L) appendDialog(DialogEntry(++dialogSeq, fromSelfvoy = false, text = text.trim()))
        guard {
            try {
                val result = client.agentAsk(text.trim(), chatId)
                val reply = result.str("reply").ifEmpty { "Готово" }
                _askReply.value = reply
                val actions = result.arr("results").mapNotNull { element ->
                    val item = element as? JsonObject ?: return@mapNotNull null
                    if (!item.bool("requires_confirmation")) return@mapNotNull null
                    DialogAction(item.long("confirmation_id"), item.str("summary"), item.str("tool"))
                }
                if (chatId == 0L) {
                    appendDialog(DialogEntry(++dialogSeq, fromSelfvoy = true, text = reply, actions = actions,
                        hasA2a = result?.get("a2a") is JsonObject))
                } else {
                    loadHistory()
                }
            } catch (error: SelfvoyError) {
                if (chatId == 0L) appendDialog(DialogEntry(++dialogSeq, fromSelfvoy = true, text = describe(error), error = true))
                throw error
            }
            refreshChats(); refreshTasks(); refreshConfirmations(); refreshNotifications(); refreshMemory()
        }
    }

    private fun appendDialog(entry: DialogEntry) { _dialog.value = _dialog.value + entry }

    fun refreshMemory() = scope.launch {
        runCatching {
            _memory.value = client.memoryList().arr("entries").mapNotNull { element ->
                val entry = element as? JsonObject ?: return@mapNotNull null
                MemoryEntry(entry.str("kind"), entry.str("text"))
            }
        }
    }

    // ------------------------------------------------------------- переговоры

    /** Один раунд A2A (agent.negotiate). Раунды с тем же собеседником считаются подряд. */
    fun negotiate(target: String, topic: String, durationMinutes: Int, windowHours: Int) = scope.launch {
        if (target.isBlank()) return@launch
        guard {
            val result = client.agentNegotiate(target.trim(), topic.trim(), durationMinutes, windowHours)
            val round = 1 + _negotiationHistory.value.takeWhile { it.target == target.trim() }.size
            fun slotOf(element: kotlinx.serialization.json.JsonElement?): Slot? =
                (element as? JsonObject)?.let { Slot(it.str("start"), it.str("end")) }
            val place = result.obj("place")?.let { p ->
                Place(p.str("name"), p.str("city"),
                    p.arr("tags").mapNotNull { (it as? JsonPrimitive)?.content },
                    p["rating"]?.jsonPrimitive?.content?.toDoubleOrNull()
                        ?.let { String.format(java.util.Locale.US, "%.1f", it) } ?: "")
            }
            val negotiation = Negotiation(
                target = target.trim(), topic = topic.trim(), durationMinutes = durationMinutes,
                windowHours = windowHours, round = round, at = System.currentTimeMillis(),
                accepted = result.bool("accepted"), slot = slotOf(result?.get("slot")), place = place,
                counterSlots = result.arr("counter_slots").mapNotNull { slotOf(it) },
                message = result.str("message"),
            )
            _negotiation.value = negotiation
            _negotiationHistory.value = listOf(negotiation) + _negotiationHistory.value
            _tab.value = Tab.NEGOTIATIONS
            refreshNotifications()
        }
    }

    fun clearNegotiation() { _negotiation.value = null }

    /**
     * «Подтвердить и забронировать»: book_table по кнопке пользователя и встреча в задачи
     * (напоминание за полчаса). Если пользователь запретил бронирование — не обходим.
     */
    fun bookNegotiation() = scope.launch {
        val current = _negotiation.value ?: return@launch
        val slot = current.slot ?: return@launch
        val place = current.place ?: return@launch
        if (_permissions.value.firstOrNull { it.tool == "book_table" }?.mode == "deny") {
            _errorText.value = "Бронирование запрещено в разрешениях"
            return@launch
        }
        guard {
            val result = client.toolRun("book_table", buildJsonObject {
                put("place", place.name)
                put("at", slot.start)
                put("people", 2)
            })
            if (result.bool("requires_confirmation")) {
                // Режим «ask»: сервер ничего не забронировал, а создал отложенное действие —
                // встречу в задачи не ставим, ждём подтверждения во «Входящих».
                refreshConfirmations()
                _errorText.value = "Бронирование ждёт вашего подтверждения"
                return@guard
            }
            val booked = current.copy(booking = result.str("confirmation").ifEmpty { "ok" })
            _negotiation.value = booked
            _negotiationHistory.value = _negotiationHistory.value.map { if (it.at == current.at) booked else it }
            val remind = runCatching {
                java.time.Instant.parse(slot.start).minusSeconds(30 * 60).toString()
            }.getOrDefault("")
            client.taskCreate("Встреча: ${current.topic.ifEmpty { "переговоры" }} — ${place.name}",
                remindAt = remind, dueAt = slot.start)
            refreshTasks()
            _errorText.value = "Столик забронирован: ${place.name}"
        }
    }

    // ------------------------------------------------------------- люди

    fun searchUsers(query: String) = scope.launch {
        if (query.trim().length < 2) { _userResults.value = emptyList(); return@launch }
        runCatching {
            _userResults.value = client.usersSearch(query.trim()).arr("users").mapNotNull { element ->
                val user = element as? JsonObject ?: return@mapNotNull null
                // users.search не отдаёт email: только имя, аватар и «в сети».
                FoundUser(user.long("id"), user.str("display_name").ifEmpty { "Пользователь Selfvoy" }, user.bool("online"))
            }.filter { it.id != _userId.value }
        }
    }

    fun consumePendingAsk() { _pendingAsk.value = null }
    fun consumeWantVoice() { _wantVoice.value = false }
    fun consumeSignupRef() { _signupRef.value = null }
    fun requestVoice() { _wantVoice.value = true }
    fun selectTab(value: Tab) {
        _tab.value = value
        if (value != Tab.CONTACTS) _openChatId.value = 0
        when (value) {
            Tab.SETTINGS -> { refreshPermissions(); refreshIntegrations(); refresh2faStatus(); refreshPrivacy(); refreshReferral() }
            Tab.HOME, Tab.BOOKING -> refreshBooking()
            Tab.MORE -> refreshReferral()
            Tab.PACKAGES -> { refreshPackages(); if (_bookingPages.value == null) refreshBooking() }
            Tab.INTEGRATIONS -> { refreshTelegram(); refreshPayments(); refreshCalendarFeed(); refreshIntegrations() }
            Tab.CONFIRMATIONS -> refreshConfirmations()
            Tab.NOTICES -> refreshNotifications()
            Tab.TASKS -> refreshTasks()
            else -> Unit
        }
    }

    // ------------------------------------------------------------- чаты

    fun refreshChats() = scope.launch {
        guard {
            val payload = client.chatList()
            _chats.value = payload.arr("chats").mapNotNull { element ->
                val chat = element as? JsonObject ?: return@mapNotNull null
                val members = chat.arr("members").mapNotNull { m ->
                    val member = m as? JsonObject ?: return@mapNotNull null
                    Member(member.long("user_id"), member.str("display_name").ifEmpty { member.str("email") },
                        member.str("email"))
                }
                val others = members.filter { it.userId != _userId.value }
                Chat(chat.long("id"),
                    chat.str("title").ifEmpty { others.joinToString { it.name }.ifEmpty { "Чат ${chat.long("id")}" } },
                    chat.str("last_message_body"), chat.str("last_message_at"), members)
            }
            // Контакты — люди из чатов, по одному на человека.
            val seen = HashSet<Long>()
            _contacts.value = _chats.value.flatMap { chat ->
                chat.members.filter { it.userId != _userId.value && seen.add(it.userId) }
                    .map { Contact(it.userId, it.name, it.email, chat.id, chat.lastBody, chat.lastAt) }
            }
        }
    }

    fun openChat(id: Long) = scope.launch {
        _openChatId.value = id
        _tab.value = Tab.CONTACTS
        guard { loadHistory() }
    }

    fun closeChat() { _openChatId.value = 0 }

    fun openChatByContact(contact: String) = scope.launch {
        guard {
            val payload = client.chatOpen(contact)
            val chat = payload.obj("chat")
            _openChatId.value = chat.long("id")
            _tab.value = Tab.CONTACTS
            refreshChats()
            loadHistory()
        }
    }

    private suspend fun loadHistory() {
        val payload = client.chatHistory(_openChatId.value)
        _messages.value = payload.arr("messages").mapNotNull { element ->
            val message = element as? JsonObject ?: return@mapNotNull null
            Message(message.long("id"), message.long("sender_id"), message.str("body"),
                message.str("kind").ifEmpty { "text" }, message.str("created_at"), message.str("sender_name"))
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
                    task.str("status"), task.str("remind_at"), task.str("due_at"), task.str("completed_at"))
            }
        }
    }

    fun createTask(title: String, remindAt: String = "", dueAt: String = "") = scope.launch {
        if (title.isBlank()) return@launch
        guard { client.taskCreate(title.trim(), remindAt = remindAt, dueAt = dueAt); refreshTasks() }
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

    // ------------------------------------------------------------- запись

    fun refreshBooking() = scope.launch {
        guard {
            val pages = client.bookingPageList()
            _bookingPages.value = pages
            _bookings.value = client.bookingList(100)
            loadFreeSlots(pages)
        }
    }

    /** Окна первой открытой страницы: ответ сервера кэшируется на минуту, ошибки не мешают ленте. */
    private suspend fun loadFreeSlots(pages: List<BookingPage>) {
        val page = pages.firstOrNull { it.active }
        _freeSlots.value = if (page == null) emptyList() else runCatching {
            client.bookingFreeSlots(page.slug).map { FreeSlot(it.start, it.end, page.id) }
        }.getOrDefault(emptyList())
    }

    /** Действие из «+»: открыть раздел, где его выполнят. */
    fun startAction(action: PendingAction) {
        _pendingAction.value = action
        selectTab(if (action == PendingAction.ISSUE_PACKAGE) Tab.PACKAGES else Tab.BOOKING)
    }

    fun consumePendingAction() { _pendingAction.value = null }

    /** pageId == null — новая страница. Возвращает итог: ошибка показывается в форме. */
    suspend fun saveBookingPage(pageId: Long?, fields: BookingPageFields): Outcome<BookingPage> = outcome {
        val page = if (pageId == null) client.bookingPageCreate(fields) else client.bookingPageUpdate(pageId, fields)
        val rest = _bookingPages.value.orEmpty().filter { it.id != page.id }
        _bookingPages.value = if (pageId == null) listOf(page) + rest
            else _bookingPages.value.orEmpty().map { if (it.id == page.id) page else it }
        if (pageId == null) loadFreeSlots(_bookingPages.value.orEmpty())
        page
    }

    fun setBookingPagePaused(page: BookingPage, paused: Boolean) = scope.launch {
        guard {
            val updated = client.bookingPageUpdate(page.id, status = if (paused) "paused" else "active")
            _bookingPages.value = _bookingPages.value.orEmpty().map { if (it.id == updated.id) updated else it }
            _errorText.value = if (paused) "Запись на паузе — новые гости не смогут записаться" else "Запись снова открыта"
        }
    }

    fun deleteBookingPage(page: BookingPage) = scope.launch {
        guard {
            client.bookingPageDelete(page.id)
            _bookingPages.value = _bookingPages.value.orEmpty().filter { it.id != page.id }
            _errorText.value = "Страница удалена. Уже сделанные записи остаются в силе."
        }
    }

    private fun putBooking(updated: Booking) {
        _bookings.value = _bookings.value.map { if (it.id == updated.id) updated else it }
    }

    /** Отмена владельцем: [occurrence] — одна встреча серии, null — все предстоящие. */
    suspend fun cancelBooking(booking: Booking, occurrence: String? = null): Outcome<Booking> = outcome {
        val updated = client.bookingOwnerCancel(booking.id, occurrence)
        putBooking(updated)
        // Счётчик «Предстоит: N» и свободные окна считает сервер — перечитываем.
        val pages = client.bookingPageList()
        _bookingPages.value = pages
        loadFreeSlots(pages)
        refreshTasks()
        updated
    }

    /** Отметка оплаты записи: paid | waived | refunded. */
    suspend fun setBookingPayment(booking: Booking, status: String): Outcome<Booking> = outcome {
        val updated = client.bookingPaymentSet(booking.id, status)
        putBooking(updated)
        _errorText.value = when (status) {
            "paid" -> "Оплата отмечена — гостю ушло письмо"
            "waived" -> "Отмечено: без оплаты"
            else -> "Отмечен возврат"
        }
        updated
    }

    /** Пост «свободные окна» для отправки через «Поделиться». */
    suspend fun slotsPost(pageId: Long, days: Int, format: String): Outcome<SlotsPost> = outcome {
        client.bookingSlotsPost(pageId, days, format)
    }

    // ------------------------------------------------- пакеты и абонементы (F5)

    fun refreshPackages() = scope.launch {
        guard { _packages.value = client.packageList(limit = 200) }
    }

    private fun putPackage(updated: PackagePurchase) {
        val list = _packages.value.orEmpty()
        _packages.value = if (list.any { it.id == updated.id }) list.map { if (it.id == updated.id) updated else it }
            else listOf(updated) + list
    }

    suspend fun issuePackage(request: PackageIssueRequest): Outcome<PackageIssued> = outcome {
        val issued = client.packageIssue(request)
        putPackage(issued.purchase)
        issued
    }

    suspend fun adjustPackage(item: PackagePurchase, delta: Int): Outcome<PackagePurchase> = outcome {
        client.packageAdjust(item.id, delta).also { putPackage(it) }
    }

    suspend fun cancelPackage(item: PackagePurchase): Outcome<PackagePurchase> = outcome {
        client.packageCancel(item.id).also { putPackage(it) }
    }

    suspend fun setPackagePayment(item: PackagePurchase, status: String): Outcome<PackagePurchase> = outcome {
        client.packagePaymentSet(item.id, status).also { putPackage(it) }
    }

    // ------------------------------------------- подключения: Telegram, ЮKassa, ICS

    fun refreshTelegram() = scope.launch {
        runCatching { client.telegramStatus() }.onSuccess { _telegram.value = it }
        // Флаг «дублировать в Telegram» живёт в prefs.notifications.
        runCatching { client.prefsGet() }.onSuccess { _prefs.value = it }
    }

    suspend fun telegramLink(): Outcome<TelegramLink> = outcome { client.telegramLink() }

    suspend fun telegramUnlink(): Outcome<Unit> = outcome {
        client.telegramUnlink()
        _telegram.value = client.telegramStatus()
        _errorText.value = "Telegram отключён"
    }

    /** Дублировать ли уведомления в Telegram; прочие настройки уведомлений сохраняются. */
    suspend fun setTelegramNotifications(enabled: Boolean): Outcome<Unit> = outcome {
        val current = (_prefs.value ?: client.prefsGet()).obj("notifications")
        _prefs.value = client.notificationsTelegramSet(enabled, current)
    }

    fun telegramNotificationsOn(): Boolean = telegramNotificationsEnabled(_prefs.value)

    fun refreshPayments() = scope.launch {
        runCatching { client.paymentsStatus() }.onSuccess { _payments.value = it }
    }

    /**
     * Подключение ЮKassa. Секретный ключ передаётся только в запрос: в сторе,
     * на диске и в журналах его нет; поле ввода очищает экран.
     */
    suspend fun connectYooKassa(shopId: String, secretKey: String): Outcome<PaymentsStatus> = outcome {
        client.paymentsConnect(YooKassaCredentials(shopId, secretKey)).also {
            _payments.value = it
            _errorText.value = "ЮKassa подключена — гости смогут платить картой и через СБП"
        }
    }

    suspend fun disconnectYooKassa(): Outcome<PaymentsStatus> = outcome {
        client.paymentsDisconnect().also {
            _payments.value = client.paymentsStatus()
            _errorText.value = "ЮKassa отключена"
        }
    }

    fun refreshCalendarFeed() = scope.launch {
        runCatching { client.calendarFeed() }.onSuccess { _calendarFeed.value = it }
    }

    /** Новая ссылка подписки (прежняя перестаёт работать); значение — готовый адрес или null. */
    suspend fun resetCalendarFeed(): Outcome<String?> = outcome {
        val feed = client.calendarFeedReset()
        _calendarFeed.value = feed
        feed.feedUrl(store.serverUrl)
    }

    suspend fun disableCalendarFeed(): Outcome<Unit> = outcome {
        _calendarFeed.value = client.calendarFeedDisable()
        _errorText.value = "Подписка на календарь выключена"
    }

    fun refreshReferral() = scope.launch {
        guard {
            val info = client.referralGet()
            _referral.value = info
            info.plan?.let { _plan.value = it }
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
            val payload = client.integrationBegin(provider, "selfvoy://oauth")
            payload.str("authorize_url")
        } catch (error: SelfvoyError) {
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
            // Сборка без google-services.json (см. docs/ANDROID.md): честно, но без путей к файлам.
            _pushStatus.value = "Push-уведомления в этой сборке не подключены"
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
            is DeepLink.Chats -> {
                // Локальная копия: id объявлен в другом модуле, smart cast невозможен.
                val chatId = link.id
                if (chatId != null) openChat(chatId) else _tab.value = Tab.CONTACTS
            }
            DeepLink.Voice -> _wantVoice.value = true
            is DeepLink.Ask -> if (link.text.isBlank()) _wantVoice.value = true else ask(link.text)
            DeepLink.Tasks -> openTabFromLink(Tab.TASKS)
            DeepLink.Notifications -> openTabFromLink(Tab.NOTICES)
            DeepLink.Settings -> openTabFromLink(Tab.SETTINGS)
            is DeepLink.Signup -> {
                // Код пригодится только новому аккаунту: после входа ссылка просто ничего не меняет.
                if (_auth.value !is AuthState.LoggedIn) _signupRef.value = link.ref
            }
            is DeepLink.OAuth -> finishIntegration(link.provider, link.code, link.state)
            null -> Unit  // неизвестная ссылка игнорируется без краша
        }
    }

    /** Раздел из deep link: после входа — как тап по вкладке (с загрузкой данных), до входа — только вкладка. */
    private fun openTabFromLink(tab: Tab) {
        if (_auth.value is AuthState.LoggedIn) selectTab(tab) else _tab.value = tab
    }

    // ------------------------------------------------------------- служебное

    private suspend fun guard(block: suspend () -> Unit) {
        _busy.value = true
        _errorText.value = null
        try {
            block()
        } catch (error: CancellationException) {
            throw error  // отмена корутины (ушли с экрана) — не ошибка для пользователя
        } catch (error: SelfvoyError) {
            if (error.code == "unauthorized" && _auth.value is AuthState.LoggedIn) {
                // Сессию отозвали без события (перепроверка сервера, другой экземпляр):
                // токены мёртвы — на вход, а не «Неверный email или пароль».
                onSessionRevoked("expired")
            } else if (error.code == "disconnected" && _auth.value !is AuthState.LoggedIn) {
                // Запрос оборвался после выхода — причину выхода не перезаписываем.
            } else {
                _errorText.value = describe(error)
            }
        } catch (error: Exception) {
            _errorText.value = error.message ?: "неизвестная ошибка"
        } finally {
            _busy.value = false
        }
    }

    /** Операция экрана с ошибкой рядом с кнопкой, а не в общем снекбаре. */
    private suspend fun <T> outcome(block: suspend () -> T): Outcome<T> = try {
        Outcome(value = block())
    } catch (error: SelfvoyError) {
        Outcome(error = describe(error), proRequired = error.isProRequired)
    } catch (error: Exception) {
        Outcome(error = error.message ?: "неизвестная ошибка")
    }

    /** Текст ошибки для человека: свой для известных кодов, иначе — текст сервера. */
    fun describe(error: SelfvoyError): String = when (error.code) {
        "unauthorized" -> "Неверный email или пароль"
        "forbidden" -> error.serverMessage.ifEmpty { "доступ запрещён" }
        "requires_2fa" -> "Нужен код двухфакторной аутентификации"
        "email_not_verified" -> "Email не подтверждён"
        // conflict бывает не только у регистрации (время заняли, запись на паузе) — текст сервера точнее.
        "conflict" -> error.serverMessage.ifEmpty { "Такой email уже зарегистрирован" }
        "not_found" -> error.serverMessage.ifEmpty { "Не найдено" }
        "disconnected" -> "Нет соединения с сервером"
        "upstream_error" -> error.serverMessage.ifEmpty { "Сервис недоступен, попробуйте позже" }
        SelfvoyErrorCode.PRO_REQUIRED -> SelfvoyErrorCode.PRO_REQUIRED_TEXT
        else -> error.serverMessage.ifEmpty { "ошибка (${error.code})" }
    }

    fun clearError() { _errorText.value = null }

    /** Короткое сообщение внизу экрана (снекбар). */
    fun notify(text: String) { _errorText.value = text }

    /** Task<T> → suspend без лишней зависимости (play-services не нужен). */
    private suspend fun <T> awaitTask(task: com.google.android.gms.tasks.Task<T>): T =
        suspendCancellableCoroutine { continuation ->
            // kotlin.Result — наш sealed Result в этом файле его затеняет.
            task.addOnSuccessListener { value -> continuation.resumeWith(kotlin.Result.success(value)) }
            task.addOnCanceledListener {
                continuation.resumeWith(
                    kotlin.Result.failure(SelfvoyError("canceled", "операция отменена")),
                )
            }
            task.addOnFailureListener { error -> continuation.resumeWith(kotlin.Result.failure(error)) }
        }
}
