// AppStore.swift — центральное состояние iOS-приложения Aura.
//
// Единственный объект, который видит SwiftUI: держит авторизацию, списки
// чатов/сообщений/задач/подтверждений/интеграций, вызывает AuraClient и
// разбирает события сервера (chat.message, task.due). Тот же контракт,
// что у Qt-клиента: сервер — источник истины, клиент — представление.
import AuraKit
import AVFoundation
import Foundation
import SwiftUI
import UIKit
import UserNotifications

enum AppTab: Hashable {
    case chats
    case tasks
    case confirmations
    case notifications
    case settings
}

@MainActor
final class AppStore: ObservableObject {

    enum AuthState: Equatable {
        case loggedOut
        case needsEmailCode(String)   // ждём код подтверждения email
        case needs2fa(String, String) // ждём TOTP-код (email, пароль для login2fa)
        case loggedIn
    }

    // MARK: Состояние для UI

    @Published var authState: AuthState = .loggedOut
    @Published var connected = false
    @Published var statusMessage = ""
    @Published var errorMessage = ""
    @Published var serverURLText = "ws://127.0.0.1:9000"

    @Published var userName = ""
    @Published var userEmail = ""

    @Published var chats: [JSONValue] = []
    @Published var currentChatId: Int64 = 0
    @Published var currentChatTitle = ""
    @Published var messages: [JSONValue] = []

    @Published var tasks: [JSONValue] = []
    @Published var confirmations: [JSONValue] = []
    @Published var permissions: [JSONValue] = []
    @Published var integrations: [JSONValue] = []
    @Published var integrationProviders: [JSONValue] = []

    // Этап 13: in-app «входящая» уведомлений и push-устройства.
    @Published var notifications: [JSONValue] = []
    @Published var unreadNotifications: Int64 = 0
    @Published var pushDevices: [JSONValue] = []

    @Published var tab: AppTab = .chats

    // Онбординг-опрос после регистрации (этап 8+): показываем, пока сервер
    // не выставил prefs.onboarded (флаг ставится при сохранении анкеты).
    @Published var needOnboarding = false
    @Published var onboardingVisible = false
    @Published var voiceOverlayVisible = false
    @Published var devEmailCode = ""      // подсказка в режиме AURA_MAIL_DRIVER=dev
    @Published var ttsEnabled = true

    let client = AuraClient()
    let keychain = KeychainStore()
    let speech = SpeechService()

    private var eventTask: Task<Void, Never>?

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
                let payload = try await client.authenticate(token: token)
                try await applyAuth(payload)
            } catch {
                keychain.delete(.accessToken)
                authState = .loggedOut
                setStatus("Сессия истекла — войдите заново")
            }
        }
    }

    private func connectTransport() throws {
        guard let url = URL(string: serverURLText),
              let scheme = url.scheme, scheme == "ws" || scheme == "wss" else {
            throw AuraError(code: "bad_request", message: "адрес сервера: ws:// или wss://")
        }
        keychain.save(serverURLText, for: .serverURL)
        try client.connect(serverURL: url)
    }

    // MARK: Auth

    func login(email: String, password: String) {
        Task {
            do {
                try connectTransport()
                let payload = try await client.login(email: email, password: password,
                                                     device: "ios",
                                                     deviceId: deviceId())
                try await applyAuth(payload)
            } catch let error as AuraError where error.code == "requires_2fa" {
                authState = .needs2fa(email, password)
            } catch {
                fail(error)
            }
        }
    }

    func register(displayName: String, email: String, password: String) {
        Task {
            do {
                try connectTransport()
                let payload = try await client.register(email: email, password: password,
                                                        displayName: displayName)
                devEmailCode = payload.string("email_code")
                authState = .needsEmailCode(email)
                setStatus("Код подтверждения отправлен на \(email)")
            } catch {
                fail(error)
            }
        }
    }

    func verifyEmail(email: String, code: String) {
        Task {
            do {
                _ = try await client.verifyEmail(email: email, code: code)
                setStatus("Email подтверждён — войдите")
                authState = .loggedOut
            } catch {
                fail(error)
            }
        }
    }

    func login2fa(email: String, password: String, code: String, trustDevice: Bool) {
        Task {
            do {
                let payload = try await client.login2fa(email: email, password: password,
                                                        code: code,
                                                        trustDevice: trustDevice,
                                                        device: "ios",
                                                        deviceId: deviceId())
                try await applyAuth(payload)
            } catch {
                fail(error)
            }
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
            messages = []
            chats = []
            tasks = []
            confirmations = []
        }
    }

    private func applyAuth(_ payload: JSONValue) async throws {
        keychain.save(payload.string("access_token"), for: .accessToken)
        keychain.save(payload.string("refresh_token"), for: .refreshToken)
        let user = payload["user"] ?? payload
        userName = user.string("display_name", default: user.string("email"))
        userEmail = user.string("email")
        authState = .loggedIn
        startEventLoop()
        await loadAll()
        // Этап 13: «входящая» уведомлений + APNs-токен (если уже получен).
        loadNotifications()
        loadPushDevices()
        await checkOnboarding()
        if !pendingPushToken.isEmpty {
            let token = pendingPushToken
            pendingPushToken = ""
            registerPushToken(token)
        }
        setStatus("Добро пожаловать, \(userName)")
    }

    private func deviceId() -> String {
        if let saved = keychain.load(.deviceId), !saved.isEmpty { return saved }
        let fresh = UUID().uuidString
        keychain.save(fresh, for: .deviceId)
        return fresh
    }

    // MARK: События сервера

    private func startEventLoop() {
        eventTask?.cancel()
        eventTask = Task { [weak self] in
            guard let self else { return }
            for await event in self.client.events() {
                await self.handleEvent(event.name, event.payload)
            }
        }
    }

    private func handleEvent(_ name: String, _ payload: JSONValue) {
        switch name {
        case AuraEventName.chatMessage:
            let chatId = payload.int("chat_id")
            if chatId == currentChatId || currentChatId == 0 {
                messages.append(payload)
            }
            loadChats()
            if ttsEnabled {
                speech.speak(payload.string("text"))
            }
        case AuraEventName.taskDue:
            loadTasks()
            setStatus("Напоминание: \(payload.string("title"))")
            if ttsEnabled {
                speech.speak("Напоминание: \(payload.string("title"))")
            }
        case AuraEventName.notificationNew:
            // In-app «входящая»: добавляем в список и обновляем счётчик.
            notifications.insert(payload, at: 0)
            unreadNotifications += 1
            updateBadge()
        default:
            break
        }
    }

    // MARK: Данные

    func loadAll() async {
        loadChats()
        loadTasks()
        loadConfirmations()
        loadPermissions()
        loadIntegrations()
    }

    // MARK: Онбординг-опрос

    func checkOnboarding() async {
        do {
            let prefs = try await client.request("prefs.get")
            let needs = !prefs.bool("onboarded")
            await MainActor.run {
                needOnboarding = needs
                onboardingVisible = needs
            }
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
        if let value = Double(budget) { payload["budget_limit"] = .number(value) }
        Task {
            do {
                _ = try await client.prefsSet(.object(payload))
                await MainActor.run {
                    needOnboarding = false
                    onboardingVisible = false
                    statusMessage = "Анкета сохранена"
                }
            } catch { fail(error) }
        }
    }

    /// «Пропустить»: пустую анкету не сохраняем, но больше не показываем.
    func skipOnboarding() {
        onboardingVisible = false
    }

    func loadChats() {
        Task {
            do {
                let payload = try await client.chatList()
                chats = payload.array("chats")
            } catch { fail(error) }
        }
    }

    func selectChat(_ chat: JSONValue) {
        currentChatId = chat.int("id")
        currentChatTitle = chat.string("title", default: chat.string("peer_email"))
        messages = []
        Task {
            do {
                let payload = try await client.chatHistory(chatId: currentChatId)
                messages = payload.array("messages")
            } catch { fail(error) }
        }
    }

    func openChat(contact: String) {
        Task {
            do {
                let payload = try await client.chatOpen(contact: contact)
                loadChats()
                selectChat(payload["chat"] ?? payload)
            } catch { fail(error) }
        }
    }

    func sendMessage(_ text: String) {
        guard !text.isEmpty, currentChatId > 0 else { return }
        Task {
            do {
                _ = try await client.chatSend(chatId: currentChatId, text: text)
            } catch { fail(error) }
        }
    }

    /// Запрос к Ауре; ответ возвращается для Siri/Shortcuts и показывается в UI.
    @discardableResult
    func askAura(_ message: String) async -> String {
        do {
            let payload = try await client.agentAsk(chatId: currentChatId, message: message)
            let reply = payload.string("reply")
            setStatus(reply.isEmpty ? "Аура выполнила запрос" : reply)
            if ttsEnabled { speech.speak(reply) }
            if currentChatId > 0 {
                // Сообщения придут событием chat.message — обновим историю.
                let history = try await client.chatHistory(chatId: currentChatId)
                messages = history.array("messages")
            }
            return reply
        } catch {
            fail(error)
            return ""
        }
    }

    func loadTasks() {
        Task {
            do {
                let payload = try await client.tasksList()
                tasks = payload.array("tasks")
            } catch { fail(error) }
        }
    }

    func createTask(title: String, notes: String, remindAt: String) {
        Task {
            do {
                _ = try await client.taskCreate(title: title, notes: notes, remindAt: remindAt)
                loadTasks()
                setStatus("Задача создана")
            } catch { fail(error) }
        }
    }

    func setTaskStatus(id: Int64, status: String) {
        Task {
            do {
                _ = try await client.taskSetStatus(id: id, status: status)
                loadTasks()
            } catch { fail(error) }
        }
    }

    func deleteTask(id: Int64) {
        Task {
            do {
                _ = try await client.taskDelete(id: id)
                loadTasks()
            } catch { fail(error) }
        }
    }

    func loadConfirmations() {
        Task {
            do {
                let payload = try await client.confirmationsList()
                confirmations = payload.array("actions")
            } catch { fail(error) }
        }
    }

    func decideConfirmation(id: Int64, approve: Bool) {
        Task {
            do {
                _ = try await client.confirmationDecide(id: id, approve: approve)
                loadConfirmations()
            } catch { fail(error) }
        }
    }

    func loadPermissions() {
        Task {
            do {
                let payload = try await client.permissionsList()
                permissions = payload.array("tools")
            } catch { fail(error) }
        }
    }

    func setPermission(tool: String, mode: String) {
        Task {
            do {
                _ = try await client.permissionSet(tool: tool, mode: mode)
                loadPermissions()
            } catch { fail(error) }
        }
    }

    // MARK: Интеграции (этап 9)

    func loadIntegrations() {
        Task {
            do {
                let payload = try await client.integrationsList()
                integrationProviders = payload.array("providers")
                integrations = payload.array("connections")
            } catch { fail(error) }
        }
    }

    func beginIntegration(provider: String) {
        Task {
            do {
                let payload = try await client.integrationBegin(provider: provider,
                                                                redirectUri: "aura://oauth")
                guard let url = URL(string: payload.string("authorize_url")) else {
                    throw AuraError(code: "internal", message: "сервер вернул некорректную ссылку")
                }
                UIApplication.shared.open(url)
                setStatus("Разрешите Ауре доступ в открывшемся окне")
            } catch { fail(error) }
        }
    }

    func integrationCallback(provider: String, code: String, state: String) {
        Task {
            do {
                _ = try await client.integrationCallback(provider: provider, code: code, state: state)
                setStatus("Подключено: \(provider)")
                loadIntegrations()
            } catch { fail(error) }
        }
    }

    func revokeIntegration(id: Int64) {
        Task {
            do {
                _ = try await client.integrationRevoke(id: id)
                setStatus("Доступ отозван")
                loadIntegrations()
            } catch { fail(error) }
        }
    }

    func syncIntegration(id: Int64) {
        Task {
            do {
                let payload = try await client.integrationSync(id: id)
                if payload.string("provider") == "google_calendar" {
                    setStatus("Календарь синхронизирован: \(payload.int("events")) событий")
                } else {
                    setStatus("Синхронизировано: \(payload.string("profile_email"))")
                }
            } catch { fail(error) }
        }
    }

    // MARK: Уведомления и push (этап 13)

    func loadNotifications() {
        Task {
            do {
                let payload = try await client.notificationsList()
                notifications = payload.array("notifications")
                unreadNotifications = payload.int("unread")
                updateBadge()
            } catch { fail(error) }
        }
    }

    func markNotificationsRead(id: Int64 = 0) {
        Task {
            do {
                let payload = try await client.notificationsRead(id: id)
                if id == 0 {
                    notifications = notifications.map { item in
                        var copy = item
                        copy["read"] = .bool(true)
                        return copy
                    }
                } else if let index = notifications.firstIndex(where: { $0.int("id") == id }) {
                    notifications[index]["read"] = .bool(true)
                }
                unreadNotifications = payload.int("unread")
                updateBadge()
            } catch { fail(error) }
        }
    }

    func loadPushDevices() {
        Task {
            do {
                let payload = try await client.devicesPushList()
                pushDevices = payload.array("devices")
            } catch { fail(error) }
        }
    }

    func revokePushDevice(id: Int64) {
        Task {
            do {
                _ = try await client.devicesPushRevoke(id: id)
                setStatus("Устройство отозвано")
                loadPushDevices()
            } catch { fail(error) }
        }
    }

    /// APNs-токен из AppDelegate: регистрируем на сервере для push-доставки.
    func registerPushToken(_ tokenHex: String) {
        guard authState == .loggedIn, !tokenHex.isEmpty else {
            pendingPushToken = tokenHex   // зарегистрируем после входа
            return
        }
        Task {
            do {
                _ = try await client.devicesPushRegister(platform: "apns", token: tokenHex)
                loadPushDevices()
            } catch { fail(error) }
        }
    }

    private var pendingPushToken = ""

    private func updateBadge() {
        Task { @MainActor in
            UIApplication.shared.applicationIconBadgeNumber = Int(unreadNotifications)
        }
    }

    // MARK: Deep links

    func handle(url: URL) {
        guard let link = DeepLink(url: url) else { return }
        switch link {
        case .chats(let id):
            tab = .chats
            if let id, let chat = chats.first(where: { $0.int("id") == id }) {
                selectChat(chat)
            }
        case .voice:
            voiceOverlayVisible = true
        case .ask(let text):
            voiceOverlayVisible = false
            Task { await askAura(text) }
        case .tasks:
            tab = .tasks
        case .settings:
            tab = .settings
        case .oauth(let provider, let code, let state):
            integrationCallback(provider: provider, code: code, state: state)
        }
    }

    // MARK: Прочее

    func setStatus(_ message: String) {
        statusMessage = message
        errorMessage = ""
    }

    func fail(_ error: Error) {
        if let error = error as? AuraError {
            errorMessage = error.message
        } else {
            errorMessage = error.localizedDescription
        }
    }
}
