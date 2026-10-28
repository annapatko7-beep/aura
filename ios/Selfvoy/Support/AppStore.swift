// AppStore.swift — центральное состояние iOS-приложения Selfvoy.
//
// Единственный объект, который видит SwiftUI: авторизация (email-код, 2FA,
// сброс пароля), диалог с Selfvoy, переговоры Selfvoy, задачи, контакты и чаты,
// подтверждения, уведомления, разрешения и интеграции. Тот же контракт, что
// у Qt- и Android-клиентов: сервер — источник истины, клиент — представление;
// опасные действия выполняются только по кнопке пользователя.
import SelfvoyKit
import AVFoundation
import Foundation
import SwiftUI
import UIKit
import UserNotifications

/// Разделы: четыре вкладки снизу («Сегодня · Запись · ИИ · Ещё») и отдельная «+»;
/// остальные открываются из «Ещё» или шапки «Сегодня» и возвращаются кнопкой «назад».
enum AppTab: Hashable {
    case home
    case selfvoy
    case negotiations
    case tasks
    case contacts
    case settings
    case confirmations
    case notifications
    case booking
    case more
    case packages
    case integrations

    /// Вкладка нижней панели (корень навигации).
    var isRoot: Bool {
        switch self {
        case .home, .booking, .selfvoy, .more: return true
        default: return false
        }
    }
}

/// Что открыть на «Записи» по кнопке из «+» или «Сегодня».
enum BookingIntent: Hashable {
    case newPage
    case post
}

/// Какую запись показать в карточке: запись и (для серии) встреча.
struct BookingFocus: Identifiable, Hashable {
    let bookingId: Int64
    let start: String?
    var id: String { "\(bookingId)-\(start ?? "")" }
}

// MARK: - Модели экранов

struct ChatMember: Hashable {
    let userId: Int64
    let name: String
    let email: String
}

struct ChatSummary: Identifiable, Hashable {
    let id: Int64
    let title: String
    let lastBody: String
    let lastAt: String
    let members: [ChatMember]
}

struct ChatMessage: Identifiable, Hashable {
    let id: Int64
    let senderId: Int64
    let body: String
    let kind: String
    let createdAt: String
    let senderName: String
}

struct SelfvoyTask: Identifiable, Hashable {
    let id: Int64
    let title: String
    let notes: String
    let status: String
    let remindAt: String
    let dueAt: String
    let completedAt: String

    /// Когда задача: срок, иначе время напоминания.
    var whenAt: String { dueAt.isEmpty ? remindAt : dueAt }
    var isDone: Bool { status == "done" }
}

/// Собеседник: человек из чатов (кроме меня), по одному на человека.
struct Contact: Identifiable, Hashable {
    var id: Int64 { userId }
    let userId: Int64
    let name: String
    let email: String
    let chatId: Int64
    let lastBody: String
    let lastAt: String
}

/// Найденный человек: users.search не отдаёт email — только имя и «в сети».
struct FoundUser: Identifiable, Hashable {
    let id: Int64
    let name: String
    let online: Bool
}

/// Действие Selfvoy, ждущее решения, прямо под его репликой.
struct DialogAction: Hashable {
    let confirmationId: Int64
    let summary: String
    let tool: String
}

struct DialogEntry: Identifiable, Hashable {
    let id: Int
    let fromSelfvoy: Bool
    let text: String
    var error = false
    var actions: [DialogAction] = []
    var hasA2a = false
}

struct Slot: Hashable {
    let start: String
    let end: String
}

struct Place: Hashable {
    let name: String
    let city: String
    let tags: [String]
    let rating: String
}

/// Один раунд agent.negotiate: запрос рядом с ответом сервера.
struct Negotiation: Identifiable, Hashable {
    let id: UUID
    let target: String
    let topic: String
    let durationMinutes: Int
    let windowHours: Int
    let round: Int
    let accepted: Bool
    let slot: Slot?
    let place: Place?
    let counterSlots: [Slot]
    let message: String
    var booking: String?
}

struct MemoryEntry: Hashable {
    let kind: String
    let text: String
}

struct PendingAction: Identifiable, Hashable {
    let id: Int64
    let tool: String
    let summary: String
}

struct Notice: Identifiable, Hashable {
    let id: Int64
    let kind: String
    let title: String
    let body: String
    var read: Bool
    let createdAt: String
    /// Данные уведомления (booking_id, status оплаты, left…).
    var payload: JSONValue = .emptyObject
}

struct PushDevice: Identifiable, Hashable {
    let id: Int64
    let platform: String
    let enabled: Bool
}

struct PermissionRow: Identifiable, Hashable {
    var id: String { tool }
    let tool: String
    let description: String
    let dangerous: Bool
    let mode: String
}

struct IntegrationProvider: Identifiable, Hashable {
    var id: String { provider }
    let provider: String
    let name: String
    let description: String
}

struct ConnectionRow: Identifiable, Hashable {
    let id: Int64
    let provider: String
    let account: String
    let status: String
}

// MARK: - Стор

@MainActor
final class AppStore: ObservableObject {

    enum AuthState: Equatable {
        case loggedOut
        case needsEmailCode(String)   // ждём код подтверждения email
        case needs2fa(String, String) // ждём TOTP-код (email, пароль для login2fa)
        case needsResetCode(String)   // ждём код сброса пароля
        case loggedIn
    }

    /// После пяти раундов без согласия решает человек (docs/A2A_PROTOCOL.md).
    static let maxRounds = 5

    // MARK: Состояние для UI

    @Published var authState: AuthState = .loggedOut
    @Published var connected = false
    @Published var statusMessage = ""
    @Published var errorMessage = ""
    @Published var serverURLText = "ws://127.0.0.1:9000"
    @Published private(set) var busyCount = 0
    var busy: Bool { busyCount > 0 }

    @Published var userId: Int64 = 0
    @Published var userName = ""
    @Published var userEmail = ""
    @Published var userBio = ""
    /// Ошибка последнего сохранения профиля (показывается под полями).
    @Published var profileError = ""
    @Published var savingProfile = false

    @Published var tab: AppTab = .home
    /// Вкладка, в которую ведёт «назад» из вложенного раздела.
    @Published var rootTab: AppTab = .home
    /// Окно «+» (быстрые действия).
    @Published var quickCreateVisible = false
    /// Запрос из «+»/«Сегодня»: открыть на «Записи» новую страницу или пост.
    @Published var bookingIntent: BookingIntent?
    /// Запрос из «+»: открыть «Абонементы» сразу с выдачей пакета.
    @Published var issuePackageRequested = false
    /// Deep link selfvoy://signup?ref=…: nil — ссылки не было; иначе код приглашения
    /// ("" — без валидного кода). Экран входа открывает регистрацию и забирает код.
    @Published var signupRef: String?

    @Published var dialog: [DialogEntry] = []
    @Published var memory: [MemoryEntry] = []
    @Published var prefs: JSONValue = .emptyObject
    /// prefs.privacy: что Selfvoy раскрывает чужому Selfvoy в переговорах.
    @Published var privacy = SelfvoyPrivacy()

    @Published var negotiation: Negotiation?
    @Published var negotiationHistory: [Negotiation] = []
    /// Раунды, шаги которых уже показаны: при возврате на вкладку не проигрываем заново.
    var revealedNegotiations = Set<UUID>()

    @Published var chats: [ChatSummary] = []
    @Published var contacts: [Contact] = []
    @Published var foundUsers: [FoundUser] = []
    @Published var openChatId: Int64 = 0
    @Published var messages: [ChatMessage] = []

    @Published var tasks: [SelfvoyTask] = []
    @Published var confirmations: [PendingAction] = []
    @Published var permissions: [PermissionRow] = []
    // Страницы записи (booking.*) и рефералы (referral.*); nil — ещё не загружены.
    @Published var bookingPages: [BookingPage]?
    @Published var bookings: [Booking] = []
    /// nil — записи ещё не загружались.
    @Published var bookingsLoaded = false
    /// Свободные окна первой открытой страницы (лента «Сегодня»).
    @Published var freeSlots: [FreeSlot] = []
    @Published var referral: ReferralInfo?
    /// Тариф из user.plan (F3); у старого сервера — nil.
    @Published var userPlan: SelfvoyPlan?
    /// F5: пакеты владельца (nil — не загружались).
    @Published var packages: [OwnerPackage]?
    /// F2/F4/F6: подключения.
    @Published var telegram: TelegramStatus?
    @Published var payments: PaymentsStatus?
    @Published var calendarFeed: CalendarFeed?
    /// Ссылка подписки — видна только сразу после calendar.feedReset (токен не хранится).
    @Published var calendarFeedLink: String?
    @Published var connections: [ConnectionRow] = []
    @Published var providers: [IntegrationProvider] = []

    @Published var notices: [Notice] = []
    @Published var unreadNotifications: Int64 = 0
    @Published var pushDevices: [PushDevice] = []

    @Published var twoFaEnabled = false
    @Published var twoFaSecret = ""
    @Published var recoveryCodes: [String] = []

    // Онбординг-опрос после регистрации: показываем, пока prefs.onboarded != true.
    @Published var needOnboarding = false
    @Published var voiceOverlayVisible = false
    @Published var ttsEnabled = true

    let client = SelfvoyClient()
    let keychain = KeychainStore()
    let speech = SpeechService()

    private var eventTask: Task<Void, Never>?
    private var dialogSeq = 0
    /// Ответ «С чем помогать?» из регистрации — уходит в память после первого входа.
    private var pendingFocus = ""
    private var pendingPushToken = ""

    // MARK: Запуск

    func bootstrap() {
        if let saved = keychain.load(.serverURL), !saved.isEmpty {
            serverURLText = saved
        }
        client.onStateChange = { [weak self] state in
            Task { @MainActor in
                self?.connected = state == .connected
            }
        }
        guard let token = keychain.load(.accessToken), !token.isEmpty else {
            authState = .loggedOut
            return
        }
        Task {
            do {
                try connectTransport()
                do {
                    _ = try await client.authenticate(token: token)
                } catch {
                    // Access-токен истёк или отозван — пробуем ротацию refresh-токена.
                    guard let refresh = keychain.load(.refreshToken), !refresh.isEmpty else { throw error }
                    let rotated = try await client.refresh(refreshToken: refresh)
                    saveTokens(rotated)
                    _ = try await client.authenticate(token: rotated.string("token"))
                }
                // auth.me возвращает пользователя прямо в payload (без обёртки user).
                let me = try await client.me()
                try await signedIn(user: me["user"] ?? me)
            } catch {
                keychain.delete(.accessToken)
                keychain.delete(.refreshToken)
                authState = .loggedOut
                setStatus("Сессия истекла, войдите снова")
            }
        }
    }

    private func connectTransport() throws {
        guard let url = URL(string: serverURLText),
              let scheme = url.scheme, scheme == "ws" || scheme == "wss" else {
            throw SelfvoyError(code: "bad_request", message: "Адрес сервера начинается с ws:// или wss://")
        }
        keychain.save(serverURLText, for: .serverURL)
        if !client.isConnected {
            try client.connect(serverURL: url)
        }
    }

    // MARK: Авторизация

    func login(email: String, password: String) {
        run {
            try self.connectTransport()
            do {
                let payload = try await self.client.login(email: email, password: password,
                                                          device: "ios", deviceId: self.deviceId())
                try await self.applyAuth(payload)
            } catch let error as SelfvoyError where error.code == "requires_2fa" {
                self.authState = .needs2fa(email, password)
            } catch let error as SelfvoyError where error.code == "email_not_verified" {
                self.authState = .needsEmailCode(email)
            }
        }
    }

    /// `ref` — код приглашения (необязательно); неверный код клиент не отправляет.
    func register(displayName: String, email: String, password: String, focus: String, ref: String = "") {
        pendingFocus = focus.trimmingCharacters(in: .whitespacesAndNewlines)
        run {
            try self.connectTransport()
            _ = try await self.client.register(email: email, password: password, displayName: displayName,
                                               ref: ref)
            self.authState = .needsEmailCode(email)
            self.setStatus("Мы отправили код подтверждения на \(email)")
        }
    }

    func verifyEmail(email: String, code: String) {
        run {
            _ = try await self.client.verifyEmail(email: email, code: code)
            self.authState = .loggedOut
            self.setStatus("Email подтверждён, теперь можно войти")
        }
    }

    func resendCode(email: String) {
        run {
            _ = try await self.client.resendCode(email: email)
            self.setStatus("Код отправлен ещё раз")
        }
    }

    func forgotPassword(email: String) {
        let address = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !address.isEmpty else {
            errorMessage = "Введите email"
            return
        }
        run {
            try self.connectTransport()
            _ = try await self.client.forgotPassword(email: address)
            self.authState = .needsResetCode(address)
            self.setStatus("Если такой аккаунт есть, код отправлен на \(address)")
        }
    }

    func resetPassword(email: String, code: String, newPassword: String) {
        run {
            _ = try await self.client.resetPassword(email: email,
                                                    code: code.trimmingCharacters(in: .whitespaces),
                                                    newPassword: newPassword)
            self.authState = .loggedOut
            self.setStatus("Пароль изменён, войдите с новым паролем")
        }
    }

    func cancelAuthFlow() {
        authState = .loggedOut
    }

    func login2fa(code: String, trustDevice: Bool) {
        guard case .needs2fa(let email, let password) = authState else { return }
        run {
            let payload = try await self.client.login2fa(email: email, password: password, code: code,
                                                         trustDevice: trustDevice, device: "ios",
                                                         deviceId: self.deviceId())
            try await self.applyAuth(payload)
        }
    }

    /// Сервер отозвал сессию этого соединения и сейчас закроет сокет (код 4401):
    /// токены мертвы — не переподключаемся с ними, а сразу возвращаемся на вход.
    private func sessionRevoked(reason: String) {
        keychain.delete(.accessToken)
        keychain.delete(.refreshToken)
        eventTask?.cancel()
        eventTask = nil
        client.disconnect()
        authState = .loggedOut
        resetData()
        switch reason {
        case "password_changed", "password_reset": errorMessage = "Пароль изменён — войдите снова"
        case "account_disabled": errorMessage = "Аккаунт заблокирован администратором"
        case "logout": errorMessage = "Вы вышли из аккаунта на другом устройстве"
        default: errorMessage = "Сессия завершена — войдите снова"
        }
    }

    func logout() {
        Task {
            _ = try? await client.logout()
            keychain.delete(.accessToken)
            keychain.delete(.refreshToken)
            eventTask?.cancel()
            eventTask = nil
            client.disconnect()
            authState = .loggedOut
            resetData()
        }
    }

    func setServerURL(_ text: String) {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard value.hasPrefix("ws://") || value.hasPrefix("wss://") else {
            errorMessage = "Адрес сервера начинается с ws:// или wss://"
            return
        }
        serverURLText = value
        keychain.save(value, for: .serverURL)
        if authState == .loggedIn {
            logout()
        } else {
            client.disconnect()
        }
        setStatus("Адрес сервера сохранён")
    }

    private func saveTokens(_ payload: JSONValue) {
        // Сервер отдаёт access-токен в поле "token" (docs/PROTOCOL.md, auth.login).
        keychain.save(payload.string("token"), for: .accessToken)
        keychain.save(payload.string("refresh_token"), for: .refreshToken)
    }

    private func applyAuth(_ payload: JSONValue) async throws {
        saveTokens(payload)
        try await signedIn(user: payload["user"] ?? payload)
    }

    private func signedIn(user: JSONValue) async throws {
        applyUser(SelfvoyUser(json: user))
        authState = .loggedIn
        tab = .home
        rootTab = .home
        startEventLoop()
        loadAll()
        await checkOnboarding()
        if !pendingFocus.isEmpty {
            let focus = pendingFocus
            pendingFocus = ""
            _ = try? await client.memoryAdd(text: "Просит помогать с: \(focus)", kind: "preference")
        }
        if !pendingPushToken.isEmpty {
            let token = pendingPushToken
            pendingPushToken = ""
            registerPushToken(token)
        }
    }

    private func applyUser(_ user: SelfvoyUser) {
        userId = user.id
        userName = user.displayName.isEmpty ? user.email : user.displayName
        userEmail = user.email
        userBio = user.bio
        userPlan = user.plan
    }

    private func deviceId() -> String {
        if let saved = keychain.load(.deviceId), !saved.isEmpty { return saved }
        let fresh = UUID().uuidString
        keychain.save(fresh, for: .deviceId)
        return fresh
    }

    private func resetData() {
        dialog = []; memory = []; prefs = .emptyObject; privacy = SelfvoyPrivacy()
        negotiation = nil; negotiationHistory = []
        chats = []; contacts = []; foundUsers = []; openChatId = 0; messages = []
        tasks = []; confirmations = []; permissions = []; connections = []; providers = []
        notices = []; unreadNotifications = 0; pushDevices = []
        bookingPages = nil; bookings = []; bookingsLoaded = false; freeSlots = []; referral = nil
        userPlan = nil; packages = nil; telegram = nil; payments = nil; calendarFeed = nil; calendarFeedLink = nil
        quickCreateVisible = false; bookingIntent = nil; issuePackageRequested = false; rootTab = .home
        twoFaEnabled = false; twoFaSecret = ""; recoveryCodes = []
        userId = 0; userName = ""; userEmail = ""; userBio = ""; profileError = ""; tab = .home
        updateBadge()
    }

    // MARK: События сервера

    private func startEventLoop() {
        eventTask?.cancel()
        eventTask = Task { [weak self] in
            guard let self else { return }
            for await event in self.client.events() {
                self.handleEvent(event.name, event.payload)
            }
        }
    }

    private func handleEvent(_ name: String, _ payload: JSONValue) {
        switch name {
        case SelfvoyEventName.chatMessage:
            loadChats()
            let chatId = payload.int("chat_id")
            if chatId != 0, chatId == openChatId {
                Task { await loadHistory() }
            }
            if ttsEnabled, payload.int("sender_id") != userId {
                speech.speak(payload.string("body"))
            }
        case SelfvoyEventName.taskDue:
            loadTasks()
            setStatus("Напоминание: \(payload.string("title"))")
            if ttsEnabled {
                speech.speak("Напоминание: \(payload.string("title"))")
            }
        case SelfvoyEventName.notificationNew:
            loadNotifications()
            let kind = payload.string("kind")
            if SelfvoyNotificationKind.touchesBookings(kind) { loadBooking() }
            if SelfvoyNotificationKind.touchesPackages(kind), packages != nil { loadPackages() }
            if SelfvoyNotificationKind.touchesPlan(kind) {
                if referral != nil { loadReferral() }
                refreshPlan()
            }
        case SelfvoyEventName.sessionRevoked:
            sessionRevoked(reason: payload.string("reason"))
        default:
            break
        }
    }

    func loadAll() {
        loadChats()
        loadTasks()
        loadConfirmations()
        loadNotifications()
        loadPermissions()
        loadIntegrations()
        load2faStatus()
        loadMemory()
        loadBooking()
    }

    func selectTab(_ value: AppTab) {
        tab = value
        if value.isRoot { rootTab = value }
        if value != .contacts { openChatId = 0 }
        switch value {
        case .settings:
            loadPermissions(); loadIntegrations(); load2faStatus(); loadPrivacy(); loadReferral()
        case .booking, .home:
            loadBooking()
        case .more:
            loadReferral(); refreshPlan()
        case .packages:
            loadPackages(); loadBooking()
        case .integrations:
            loadGrowthIntegrations(); loadPrivacy()
        case .confirmations:
            loadConfirmations()
        case .notifications:
            loadNotifications()
        case .tasks:
            loadTasks()
        case .contacts:
            loadChats(); loadPermissions()
        default:
            break
        }
    }

    // MARK: Онбординг-опрос

    func checkOnboarding() async {
        do {
            let value = try await client.prefsGet()
            applyPrefs(value)
            needOnboarding = !value.bool("onboarded")
        } catch {
            fail(error)
        }
    }

    func submitOnboarding(birthday: String, allergies: [String], diet: [String],
                          transport: String, city: String, budget: String) {
        var payload: [String: JSONValue] = ["transport": .string(transport)]
        if !birthday.isEmpty { payload["birthday"] = .string(birthday) }
        if !allergies.isEmpty { payload["allergies"] = .array(allergies.map { .string($0) }) }
        if !diet.isEmpty { payload["diet"] = .array(diet.map { .string($0) }) }
        if !city.isEmpty { payload["city"] = .string(city) }
        if let value = Double(budget.replacingOccurrences(of: ",", with: ".")) {
            payload["budget_limit"] = .number(value)
        }
        run {
            _ = try await self.client.prefsSet(.object(payload))
            self.needOnboarding = false
            self.setStatus("Анкета сохранена")
            await self.checkOnboarding()
        }
    }

    /// «Пропустить»: пустую анкету не сохраняем, но больше не показываем.
    func skipOnboarding() {
        needOnboarding = false
    }

    // MARK: Профиль

    /// profile.update: имя (1–80) и «о себе» (≤300). Уходят только изменённые поля;
    /// ответ сервера (user) — новое состояние профиля. onSaved — после успеха.
    func updateProfile(displayName: String, bio: String, onSaved: @escaping () -> Void = {}) {
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        let about = bio.trimmingCharacters(in: .whitespacesAndNewlines)
        if let problem = SelfvoyProfileRules.validate(displayName: name, bio: about) {
            profileError = problem
            return
        }
        let newName: String? = name == userName ? nil : name
        let newBio: String? = about == userBio ? nil : about
        guard newName != nil || newBio != nil else {
            profileError = ""
            onSaved()
            return
        }
        profileError = ""
        savingProfile = true
        run {
            defer { self.savingProfile = false }
            do {
                let user = try await self.client.updateProfile(displayName: newName, bio: newBio)
                self.applyUser(user)
                self.setStatus("Профиль сохранён")
                onSaved()
            } catch {
                self.profileError = self.describe(error)
                throw error
            }
        }
    }

    // MARK: Приватность переговоров

    private func applyPrefs(_ value: JSONValue) {
        prefs = value
        privacy = SelfvoyPrivacy(preferences: value)
    }

    func loadPrivacy() {
        Task {
            guard let value = try? await client.prefsGet() else { return }
            applyPrefs(value)
        }
    }

    /// Переключатель сохраняется сразу (prefs.set {"privacy": {flag: value}});
    /// при ошибке сервера возвращаем прежнее значение и показываем ошибку.
    func setPrivacy(_ flag: SelfvoyPrivacy.Flag, _ value: Bool) {
        let previous = privacy[flag]
        guard previous != value else { return }
        privacy[flag] = value
        run {
            do {
                self.applyPrefs(try await self.client.privacySet([flag: value]))
            } catch {
                self.privacy[flag] = previous
                throw error
            }
        }
    }

    // MARK: Selfvoy

    /// Запрос к Selfvoy из «Моего Selfvoy», «Главной» и голоса. Реплика и ответ ложатся в
    /// диалог; опасные действия из results[] — кнопками «Подтвердить / Отклонить».
    func ask(_ text: String, chatId: Int64 = 0) {
        Task { await askSelfvoy(text, chatId: chatId) }
    }

    @discardableResult
    func askSelfvoy(_ text: String, chatId: Int64 = 0) async -> String {
        let message = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !message.isEmpty else { return "" }
        if chatId == 0 { appendDialog(fromSelfvoy: false, text: message) }
        busyCount += 1
        defer { busyCount -= 1 }
        do {
            let result = try await client.agentAsk(chatId: chatId, message: message)
            let reply = result.string("reply").isEmpty ? "Готово" : result.string("reply")
            let actions: [DialogAction] = result.array("results").compactMap { item -> DialogAction? in
                guard item.bool("requires_confirmation") else { return nil }
                return DialogAction(confirmationId: item.int("confirmation_id"),
                                    summary: item.string("summary"), tool: item.string("tool"))
            }
            if chatId == 0 {
                var hasA2a = false
                if case .object = result["a2a"] { hasA2a = true }
                appendDialog(fromSelfvoy: true, text: reply, actions: actions, hasA2a: hasA2a)
            } else {
                await loadHistory()
            }
            if ttsEnabled { speech.speak(reply) }
            loadChats(); loadTasks(); loadConfirmations(); loadNotifications(); loadMemory()
            return reply
        } catch {
            if chatId == 0 { appendDialog(fromSelfvoy: true, text: describe(error), error: true) }
            fail(error)
            return ""
        }
    }

    private func appendDialog(fromSelfvoy: Bool, text: String, error: Bool = false,
                              actions: [DialogAction] = [], hasA2a: Bool = false) {
        dialogSeq += 1
        dialog.append(DialogEntry(id: dialogSeq, fromSelfvoy: fromSelfvoy, text: text, error: error,
                                  actions: actions, hasA2a: hasA2a))
    }

    func loadMemory() {
        Task {
            guard let payload = try? await client.memoryList() else { return }
            memory = payload.array("entries").map {
                MemoryEntry(kind: $0.string("kind"), text: $0.string("text"))
            }
        }
    }

    // MARK: Переговоры

    /// Один раунд A2A (agent.negotiate). Раунды с тем же собеседником считаются подряд.
    func negotiate(target: String, topic: String, durationMinutes: Int, windowHours: Int) {
        let who = target.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !who.isEmpty else { return }
        let subject = topic.trimmingCharacters(in: .whitespacesAndNewlines)
        run {
            let result = try await self.client.agentNegotiate(target: who, topic: subject,
                                                              durationMinutes: durationMinutes,
                                                              windowHours: windowHours)
            let round = 1 + self.negotiationHistory.prefix(while: { $0.target == who }).count
            let item = Negotiation(id: UUID(), target: who, topic: subject,
                                   durationMinutes: durationMinutes, windowHours: windowHours,
                                   round: round, accepted: result.bool("accepted"),
                                   slot: Self.slot(result["slot"]), place: Self.place(result["place"]),
                                   counterSlots: result.array("counter_slots").compactMap { Self.slot($0) },
                                   message: result.string("message"))
            self.negotiation = item
            self.negotiationHistory.insert(item, at: 0)
            self.tab = .negotiations
            self.loadNotifications()
        }
    }

    private static func slot(_ value: JSONValue?) -> Slot? {
        guard let value, case .object = value else { return nil }
        return Slot(start: value.string("start"), end: value.string("end"))
    }

    private static func place(_ value: JSONValue?) -> Place? {
        guard let value, case .object = value else { return nil }
        let rating = value["rating"]?.doubleValue.map { String(format: "%.1f", $0) } ?? ""
        return Place(name: value.string("name"), city: value.string("city"),
                     tags: value.array("tags").compactMap(\.stringValue), rating: rating)
    }

    func clearNegotiation() {
        negotiation = nil
    }

    /// «Подтвердить и забронировать»: book_table по кнопке пользователя и встреча в задачи
    /// с напоминанием за полчаса. Если бронирование запрещено в разрешениях — не обходим.
    func bookNegotiation() {
        guard let current = negotiation, let slot = current.slot, let place = current.place else { return }
        if permissions.first(where: { $0.tool == "book_table" })?.mode == "deny" {
            errorMessage = "Бронирование запрещено в разрешениях"
            return
        }
        run {
            let result = try await self.client.toolRun(tool: "book_table", args: [
                "place": .string(place.name),
                "at": .string(slot.start),
                "people": .from(2)
            ])
            if result.bool("requires_confirmation") {
                // Режим «ask»: сервер ничего не забронировал, а создал отложенное действие —
                // встречу в задачи не ставим, ждём подтверждения во «Входящих».
                self.loadConfirmations()
                self.setStatus("Бронирование ждёт вашего подтверждения")
                return
            }
            var booked = current
            booked.booking = result.string("confirmation").isEmpty ? "ok" : result.string("confirmation")
            self.negotiation = booked
            self.negotiationHistory = self.negotiationHistory.map { $0.id == current.id ? booked : $0 }
            let remind = Format.parseISO(slot.start).map { Format.isoString($0.addingTimeInterval(-30 * 60)) } ?? ""
            let topic = current.topic.isEmpty ? "переговоры" : current.topic
            _ = try await self.client.taskCreate(title: "Встреча: \(topic) — \(place.name)",
                                                 remindAt: remind, dueAt: slot.start)
            self.loadTasks()
            self.setStatus("Столик забронирован: \(place.name)")
        }
    }

    // MARK: Люди и чаты

    func searchUsers(_ query: String) {
        let text = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard text.count >= 2 else {
            foundUsers = []
            return
        }
        Task {
            guard let payload = try? await client.usersSearch(query: text) else { return }
            foundUsers = payload.array("users").map {
                FoundUser(id: $0.int("id"),
                          name: $0.string("display_name", default: "Пользователь Selfvoy"),
                          online: $0.bool("online"))
            }.filter { $0.id != userId }
        }
    }

    func loadChats() {
        run {
            let payload = try await self.client.chatList()
            let me = self.userId
            self.chats = payload.array("chats").map { chat in
                let members = chat.array("members").map {
                    ChatMember(userId: $0.int("user_id"),
                               name: $0.string("display_name", default: $0.string("email")),
                               email: $0.string("email"))
                }
                let others = members.filter { $0.userId != me }.map(\.name).joined(separator: ", ")
                let title = chat.string("title").isEmpty
                    ? (others.isEmpty ? "Чат \(chat.int("id"))" : others) : chat.string("title")
                return ChatSummary(id: chat.int("id"), title: title,
                                   lastBody: chat.string("last_message_body"),
                                   lastAt: chat.string("last_message_at"), members: members)
            }
            var seen = Set<Int64>()
            self.contacts = self.chats.flatMap { chat in
                chat.members.filter { $0.userId != me && seen.insert($0.userId).inserted }.map {
                    Contact(userId: $0.userId, name: $0.name, email: $0.email, chatId: chat.id,
                            lastBody: chat.lastBody, lastAt: chat.lastAt)
                }
            }
        }
    }

    func openChat(id: Int64) {
        openChatId = id
        messages = []
        tab = .contacts
        run { await self.loadHistory() }
    }

    func openChat(contact: String) {
        run {
            let payload = try await self.client.chatOpen(contact: contact)
            let chat = payload["chat"] ?? payload
            self.openChatId = chat.int("id")
            self.messages = []
            self.tab = .contacts
            self.loadChats()
            await self.loadHistory()
        }
    }

    func closeChat() {
        openChatId = 0
    }

    private func loadHistory() async {
        let chatId = openChatId
        guard chatId > 0, let payload = try? await client.chatHistory(chatId: chatId) else { return }
        guard chatId == openChatId else { return }
        messages = payload.array("messages").map {
            ChatMessage(id: $0.int("id"), senderId: $0.int("sender_id"), body: $0.string("body"),
                        kind: $0.string("kind", default: "text"), createdAt: $0.string("created_at"),
                        senderName: $0.string("sender_name"))
        }
        _ = try? await client.chatRead(chatId: chatId)
    }

    func sendMessage(_ text: String) {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !body.isEmpty, openChatId > 0 else { return }
        run {
            _ = try await self.client.chatSend(chatId: self.openChatId, text: body)
            await self.loadHistory()
        }
    }

    // MARK: Задачи

    func loadTasks() {
        run {
            let payload = try await self.client.tasksList()
            self.tasks = payload.array("tasks").map {
                SelfvoyTask(id: $0.int("id"), title: $0.string("title"), notes: $0.string("notes"),
                         status: $0.string("status"), remindAt: $0.string("remind_at"),
                         dueAt: $0.string("due_at"), completedAt: $0.string("completed_at"))
            }
        }
    }

    func createTask(title: String, notes: String = "", remindAt: String = "", dueAt: String = "") {
        let text = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        run {
            _ = try await self.client.taskCreate(title: text, notes: notes, remindAt: remindAt, dueAt: dueAt)
            self.loadTasks()
            self.setStatus("Задача создана")
        }
    }

    /// status: complete | cancel | reopen.
    func setTaskStatus(id: Int64, status: String) {
        run {
            _ = try await self.client.taskSetStatus(id: id, status: status)
            self.loadTasks()
        }
    }

    func deleteTask(id: Int64) {
        run {
            _ = try await self.client.taskDelete(id: id)
            self.loadTasks()
        }
    }

    // MARK: Подтверждения и разрешения

    func loadConfirmations() {
        run {
            let payload = try await self.client.confirmationsList()
            self.confirmations = payload.array("actions").map {
                PendingAction(id: $0.int("id"), tool: $0.string("tool"), summary: $0.string("summary"))
            }
        }
    }

    func decideConfirmation(id: Int64, approve: Bool) {
        run {
            _ = try await self.client.confirmationDecide(id: id, approve: approve)
            self.setStatus(approve ? "Подтверждено" : "Отклонено")
            self.loadConfirmations(); self.loadChats(); self.loadNotifications()
        }
    }

    // MARK: Страницы записи и рефералы

    func loadBooking() {
        run {
            let pages = try await self.client.bookingPageList()
            self.bookingPages = pages
            self.bookings = try await self.client.bookingList(limit: 200)
            self.bookingsLoaded = true
            await self.loadFreeSlots(pages: pages)
        }
    }

    /// Свободные окна первой открытой страницы — те же, что видит гость (ответ
    /// сервера кэшируется на минуту). Ошибка AI-сервиса — просто без окон.
    private func loadFreeSlots(pages: [BookingPage]) async {
        guard let page = pages.first(where: { $0.isActive }) else {
            freeSlots = []
            return
        }
        freeSlots = (try? await client.bookingFreeSlots(page: page)) ?? []
    }

    /// Задачи со сроком для ленты дня.
    var dayTasks: [DayTaskInput] {
        tasks.compactMap { task in
            guard task.status == "pending", let at = Format.parseISO(task.whenAt) else { return nil }
            return DayTaskInput(id: task.id, title: task.title, at: at)
        }
    }

    func page(id: Int64) -> BookingPage? {
        bookingPages?.first { $0.id == id }
    }

    /// Публичная ссылка страницы для текста «Предложить» и поста.
    func publicLink(_ page: BookingPage) -> String? {
        page.url.isEmpty ? nil : page.url
    }

    private func put(_ booking: Booking) {
        if bookings.contains(where: { $0.id == booking.id }) {
            bookings = bookings.map { $0.id == booking.id ? booking : $0 }
        } else {
            bookings.insert(booking, at: 0)
        }
    }

    /// booking.paymentSet из карточки записи; ошибка — текстом для окна.
    func setBookingPayment(_ booking: Booking, _ mark: OwnerPaymentMark) async throws {
        let updated = try await client.bookingPaymentSet(id: booking.id, status: mark)
        put(updated)
        switch mark {
        case .paid: setStatus("Оплата отмечена — гостю ушло письмо «Оплата получена»")
        case .waived: setStatus("Отмечено: без оплаты")
        case .refunded: setStatus("Отмечен возврат")
        }
    }

    /// Отмена одной встречи серии (occurrence) или всех предстоящих (nil).
    func cancelBooking(_ booking: Booking, occurrence: String?) async throws {
        let updated = try await client.bookingOwnerCancel(id: booking.id, occurrence: occurrence)
        put(updated)
        setStatus(occurrence == nil ? "Запись отменена — гость получит письмо" : "Встреча отменена — гость получит письмо")
        bookingPages = try? await client.bookingPageList()
        loadTasks()
    }

    /// Создание или правка страницы со всеми полями (F1–F5). Правка уходит только
    /// изменёнными полями; pro_required и прочие ошибки — исключением для формы.
    func saveBookingPage(page: BookingPage?, fields: BookingPageFields) async throws {
        let saved: BookingPage
        if let page {
            let changes = fields.changes(from: page.fields)
            if changes.isEmpty { return }
            saved = try await client.bookingPageUpdate(id: page.id, fields: fields, original: page.fields)
            bookingPages = (bookingPages ?? []).map { $0.id == saved.id ? saved : $0 }
        } else {
            saved = try await client.bookingPageCreate(fields)
            bookingPages = [saved] + (bookingPages ?? []).filter { $0.id != saved.id }
        }
        setStatus(page == nil ? "Страница создана — поделитесь ссылкой" : "Сохранено")
        await loadFreeSlots(pages: bookingPages ?? [])
    }

    func slotsPost(page: BookingPage, days: Int, format: SlotsPostFormat) async throws -> SlotsPost {
        try await client.bookingSlotsPost(pageId: page.id, days: days, format: format)
    }

    // MARK: Пакеты и абонементы (F5)

    func loadPackages() {
        run { self.packages = try await self.client.packageList(limit: 200) }
    }

    private func put(_ package: OwnerPackage) {
        var list = packages ?? []
        if list.contains(where: { $0.id == package.id }) {
            list = list.map { $0.id == package.id ? package : $0 }
        } else {
            list.insert(package, at: 0)
        }
        packages = list
    }

    func issuePackage(_ request: PackageIssueRequest) async throws -> PackageIssueResult {
        let result = try await client.packageIssue(request)
        put(result.package)
        return result
    }

    func adjustPackage(_ package: OwnerPackage, delta: Int) async throws {
        put(try await client.packageAdjust(id: package.id, delta: delta))
    }

    func cancelPackage(_ package: OwnerPackage) async throws {
        put(try await client.packageCancel(id: package.id))
        setStatus("Абонемент отменён — клиент получит письмо")
    }

    func setPackagePayment(_ package: OwnerPackage, _ mark: OwnerPaymentMark) async throws {
        put(try await client.packagePaymentSet(id: package.id, status: mark))
    }

    // MARK: Подключения: Telegram, ЮKassa, календарь (F2, F4, F6)

    func loadGrowthIntegrations() {
        Task {
            if let value = try? await client.telegramStatus() { telegram = value }
            if let value = try? await client.paymentsStatus() { payments = value }
            if let value = try? await client.calendarFeed() { calendarFeed = value }
        }
    }

    /// Ссылка на бота (15 минут) — открывается в Telegram.
    func telegramConnectURL() async throws -> URL {
        let link = try await client.telegramLink()
        guard let url = URL(string: link.url) else {
            throw SelfvoyError(code: "bad_request", message: "Сервер вернул неверную ссылку на бота")
        }
        return url
    }

    func telegramUnlink() {
        run {
            _ = try await self.client.telegramUnlink()
            self.telegram = try await self.client.telegramStatus()
            self.setStatus("Telegram отключён")
        }
    }

    /// `prefs.notifications.telegram` (по умолчанию true).
    var telegramNotifications: Bool {
        prefs["notifications"]?["telegram"]?.boolValue ?? true
    }

    func setTelegramNotifications(_ enabled: Bool) {
        run {
            let current = try await self.client.prefsGet()
            let payload = SelfvoyClient.telegramNotificationsPayload(current: current, enabled: enabled)
            self.applyPrefs(try await self.client.prefsSet(payload))
        }
    }

    /// Секрет ЮKassa живёт только в аргументе этого вызова: не сохраняется и не логируется.
    func connectPayments(shopId: String, secretKey: String) async throws {
        payments = try await client.paymentsConnect(shopId: shopId, secretKey: secretKey)
        setStatus("ЮKassa подключена — гости смогут платить картой и через СБП")
    }

    func disconnectPayments() {
        run {
            self.payments = try await self.client.paymentsDisconnect()
            self.setStatus("ЮKassa отключена")
        }
    }

    /// Новая ссылка подписки: прежняя перестаёт работать во всех календарях.
    func resetCalendarFeed() async throws {
        let feed = try await client.calendarFeedReset()
        calendarFeed = feed
        calendarFeedLink = feed.resolvedURL(serverURL: serverURLText)
        if calendarFeedLink == nil {
            throw SelfvoyError(code: "bad_request",
                               message: "Ссылку не собрать: адрес сервера неизвестен. Задайте на сервере SELFVOY_SERVER_PUBLIC_URL.")
        }
    }

    func disableCalendarFeed() {
        run {
            self.calendarFeed = try await self.client.calendarFeedDisable()
            self.calendarFeedLink = nil
            self.setStatus("Подписка на календарь выключена")
        }
    }

    /// Тариф меняется наградой за приглашение — перечитываем пользователя.
    func refreshPlan() {
        Task {
            guard let me = try? await client.me() else { return }
            userPlan = SelfvoyUser(json: me["user"] ?? me).plan
        }
    }

    func setBookingPagePaused(_ page: BookingPage, paused: Bool) {
        run {
            let updated = try await self.client.bookingPageUpdate(id: page.id, status: paused ? "paused" : "active")
            self.bookingPages = (self.bookingPages ?? []).map { $0.id == updated.id ? updated : $0 }
            self.setStatus(paused ? "Запись на паузе — новые гости не смогут записаться" : "Запись снова открыта")
        }
    }

    func deleteBookingPage(_ page: BookingPage) {
        run {
            _ = try await self.client.bookingPageDelete(id: page.id)
            self.bookingPages = (self.bookingPages ?? []).filter { $0.id != page.id }
            self.setStatus("Страница удалена. Уже сделанные записи остаются в силе.")
        }
    }

    func loadReferral() {
        run {
            let info = try await self.client.referralGet()
            self.referral = info
            if let plan = info.plan { self.userPlan = plan }
        }
    }

    func loadPermissions() {
        run {
            let payload = try await self.client.permissionsList()
            self.permissions = payload.array("tools").map {
                PermissionRow(tool: $0.string("tool"), description: $0.string("description"),
                              dangerous: $0.bool("dangerous"), mode: $0.string("mode"))
            }
        }
    }

    func setPermission(tool: String, mode: String) {
        run {
            _ = try await self.client.permissionSet(tool: tool, mode: mode)
            self.loadPermissions()
        }
    }

    // MARK: 2FA

    func load2faStatus() {
        Task {
            guard let payload = try? await client.status2fa() else { return }
            twoFaEnabled = payload.bool("enabled")
        }
    }

    func setup2fa() {
        run {
            let payload = try await self.client.setup2fa()
            let uri = payload.string("otpauth_uri")
            self.twoFaSecret = uri.isEmpty ? payload.string("secret") : uri
        }
    }

    func confirm2fa(code: String) {
        run {
            let payload = try await self.client.confirm2fa(code: code)
            self.recoveryCodes = payload.array("recovery_codes").compactMap(\.stringValue)
            self.twoFaSecret = ""
            self.twoFaEnabled = true
        }
    }

    func disable2fa(password: String) {
        run {
            _ = try await self.client.disable2fa(password: password)
            self.twoFaEnabled = false
            self.recoveryCodes = []
        }
    }

    // MARK: Интеграции

    func loadIntegrations() {
        run {
            let payload = try await self.client.integrationsList()
            self.providers = payload.array("providers").map {
                IntegrationProvider(provider: $0.string("provider"),
                                    name: $0.string("name", default: $0.string("provider")),
                                    description: $0.string("description"))
            }
            self.connections = payload.array("connections").map {
                ConnectionRow(id: $0.int("id"), provider: $0.string("provider"),
                              account: $0.string("account"), status: $0.string("status"))
            }
        }
    }

    func beginIntegration(provider: String) {
        run {
            let payload = try await self.client.integrationBegin(provider: provider, redirectUri: "selfvoy://oauth")
            guard let url = URL(string: payload.string("authorize_url")) else {
                throw SelfvoyError(code: "internal", message: "сервер вернул некорректную ссылку")
            }
            _ = await UIApplication.shared.open(url)
            self.setStatus("Разрешите Selfvoy доступ в открывшемся окне")
        }
    }

    func integrationCallback(provider: String, code: String, state: String) {
        run {
            _ = try await self.client.integrationCallback(provider: provider, code: code, state: state)
            self.setStatus("Подключено: \(provider)")
            self.loadIntegrations()
        }
    }

    func revokeIntegration(id: Int64) {
        run {
            _ = try await self.client.integrationRevoke(id: id)
            self.setStatus("Доступ отозван")
            self.loadIntegrations()
        }
    }

    func syncIntegration(id: Int64) {
        run {
            let payload = try await self.client.integrationSync(id: id)
            if payload.string("provider") == "google_calendar" {
                self.setStatus("Календарь синхронизирован: \(payload.int("events")) событий")
            } else {
                self.setStatus("Синхронизировано: \(payload.string("profile_email"))")
            }
        }
    }

    // MARK: Уведомления и push

    func loadNotifications() {
        run {
            let payload = try await self.client.notificationsList()
            self.notices = payload.array("notifications").map {
                Notice(id: $0.int("id"), kind: $0.string("kind"), title: $0.string("title"),
                       body: $0.string("body"), read: $0.bool("read"), createdAt: $0.string("created_at"),
                       payload: $0["payload"] ?? .emptyObject)
            }
            self.unreadNotifications = payload.int("unread")
            self.updateBadge()
        }
    }

    /// id = 0 — отметить прочитанными все.
    func markNotificationsRead(id: Int64 = 0) {
        run {
            let payload = try await self.client.notificationsRead(id: id)
            self.notices = self.notices.map { notice in
                var copy = notice
                if id == 0 || notice.id == id { copy.read = true }
                return copy
            }
            self.unreadNotifications = payload.int("unread")
            self.updateBadge()
        }
    }

    func loadPushDevices() {
        run {
            let payload = try await self.client.devicesPushList()
            self.pushDevices = payload.array("devices").map {
                PushDevice(id: $0.int("id"), platform: $0.string("platform"), enabled: $0.bool("enabled"))
            }
        }
    }

    func revokePushDevice(id: Int64) {
        run {
            _ = try await self.client.devicesPushRevoke(id: id)
            self.setStatus("Устройство отозвано")
            self.loadPushDevices()
        }
    }

    /// APNs-токен из AppDelegate: регистрируем на сервере для push-доставки.
    func registerPushToken(_ tokenHex: String) {
        guard authState == .loggedIn, !tokenHex.isEmpty else {
            pendingPushToken = tokenHex   // зарегистрируем после входа
            return
        }
        run {
            _ = try await self.client.devicesPushRegister(platform: "apns", token: tokenHex)
            self.loadPushDevices()
        }
    }

    private func updateBadge() {
        UNUserNotificationCenter.current().setBadgeCount(Int(unreadNotifications)) { _ in }
    }

    // MARK: Deep links

    func handle(url: URL) {
        guard let link = DeepLink(url: url) else { return }
        switch link {
        case .chats(let id):
            if let id {
                openChat(id: id)
            } else {
                selectTab(.contacts)
            }
        case .voice:
            voiceOverlayVisible = true
        case .ask(let text):
            voiceOverlayVisible = false
            tab = .selfvoy
            ask(text)
        case .tasks:
            selectTab(.tasks)
        case .notifications:
            selectTab(.notifications)
        case .settings:
            selectTab(.settings)
        case .signup(let ref):
            // Код пригодится только новому аккаунту: после входа ссылка ничего не меняет.
            if authState != .loggedIn { signupRef = ref }
        case .oauth(let provider, let code, let state):
            integrationCallback(provider: provider, code: code, state: state)
        }
    }

    // MARK: Служебное

    /// Сетевой вызов с индикатором занятости; ошибка — в errorMessage (тост).
    private func run(_ work: @escaping @MainActor () async throws -> Void) {
        Task {
            busyCount += 1
            defer { busyCount -= 1 }
            do {
                try await work()
            } catch {
                fail(error)
            }
        }
    }

    func setStatus(_ message: String) {
        statusMessage = message
        errorMessage = ""
    }

    func fail(_ error: Error) {
        if let error = error as? SelfvoyError {
            if error.code == "unauthorized" && authState == .loggedIn {
                // Сессию отозвали без события (перепроверка сервера, другой экземпляр):
                // токены мертвы — на вход, а не «Неверный email или пароль».
                sessionRevoked(reason: "expired")
                return
            }
            // Запрос оборвался уже после выхода — причину выхода не перезаписываем.
            if error.code == "disconnected" && authState != .loggedIn { return }
        }
        errorMessage = describe(error)
    }

    /// Текст ошибки для пользователя (окна и формы показывают его у себя).
    func describe(_ error: Error) -> String {
        guard let error = error as? SelfvoyError else { return error.localizedDescription }
        switch error.code {
        case ProRequired.code: return ProRequired.text
        case "unauthorized": return "Неверный email или пароль"
        case "requires_2fa": return "Нужен код двухфакторной аутентификации"
        case "email_not_verified": return "Email не подтверждён"
        // conflict/not_found: причину знает сервер («запись уже отменена»,
        // «пользователь с таким email уже существует»…) — показываем её.
        case "conflict": return error.message.isEmpty ? "Действие уже недоступно — обновите экран" : error.message
        case "not_found": return error.message.isEmpty ? "Не найдено" : error.message
        case "disconnected": return "Нет соединения с сервером"
        case "upstream_error": return "AI-сервис недоступен, попробуйте позже"
        default: return error.message.isEmpty ? "Ошибка (\(error.code))" : error.message
        }
    }
}
