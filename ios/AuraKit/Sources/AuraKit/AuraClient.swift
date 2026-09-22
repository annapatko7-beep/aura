// AuraClient.swift — типизированный API клиента Aura поверх WS-транспорта.
//
// Методы повторяют имена WS-хендлеров сервера (auth.login, chat.send,
// agent.ask, tasks.*, integrations.* …). Ответ — payload кадра "ok";
// ошибки прилетают AuraError с серверным code/message.
import Foundation

public final class AuraClient: @unchecked Sendable {

    private let transport = WebSocketTransport()

    public init() {}

    public var isConnected: Bool { transport.state == .connected }
    public var onStateChange: ((WebSocketTransport.State) -> Void)? {
        get { transport.onStateChange }
        set { transport.onStateChange = newValue }
    }

    public func connect(serverURL: URL) throws {
        try transport.connect(url: serverURL)
    }

    public func disconnect() {
        transport.disconnect()
    }

    public func events() -> AsyncStream<WebSocketTransport.AuraEvent> {
        transport.events()
    }

    /// Универсальный запрос (payload собирается вызывающим кодом).
    public func request(_ type: String, _ payload: [String: JSONValue] = [:]) async throws -> JSONValue {
        try await transport.request(type, .object(payload))
    }

    /// Универсальный запрос с произвольным JSON-payload (например prefs.set,
    /// где поля настроек лежат прямо в корне payload).
    public func request(_ type: String, _ payload: JSONValue) async throws -> JSONValue {
        try await transport.request(type, payload)
    }

    // MARK: - Auth

    public func register(email: String, password: String, displayName: String) async throws -> JSONValue {
        try await request("auth.register", [
            "email": .string(email),
            "password": .string(password),
            "display_name": .string(displayName)
        ])
    }

    public func verifyEmail(email: String, code: String) async throws -> JSONValue {
        try await request("auth.verifyEmail", [
            "email": .string(email),
            "code": .string(code)
        ])
    }

    public func resendCode(email: String) async throws -> JSONValue {
        try await request("auth.resendCode", ["email": .string(email)])
    }

    public func login(email: String, password: String, device: String,
                      deviceId: String) async throws -> JSONValue {
        try await request("auth.login", [
            "email": .string(email),
            "password": .string(password),
            "device": .string(device),
            "device_id": .string(deviceId)
        ])
    }

    /// Завершение входа с TOTP-кодом (2FA). Сервер повторно проверяет пароль
    /// (docs/PROTOCOL.md: auth.login2fa = email + password + code).
    public func login2fa(email: String, password: String, code: String,
                         trustDevice: Bool, device: String,
                         deviceId: String) async throws -> JSONValue {
        try await request("auth.login2fa", [
            "email": .string(email),
            "password": .string(password),
            "code": .string(code),
            "trust_device": .bool(trustDevice),
            "device": .string(device),
            "device_id": .string(deviceId)
        ])
    }

    /// Повторная аутентификация соединения сохранённым access-токеном.
    public func authenticate(token: String) async throws -> JSONValue {
        try await request("auth.token", ["token": .string(token)])
    }

    public func refresh(refreshToken: String) async throws -> JSONValue {
        try await request("auth.refresh", ["refresh_token": .string(refreshToken)])
    }

    public func me() async throws -> JSONValue {
        try await request("auth.me")
    }

    public func logout() async throws -> JSONValue {
        try await request("auth.logout")
    }

    public func status2fa() async throws -> JSONValue {
        try await request("auth.status2fa")
    }

    // MARK: - Чаты и сообщения

    public func chatList() async throws -> JSONValue {
        try await request("chat.list")
    }

    public func chatOpen(contact: String) async throws -> JSONValue {
        try await request("chat.open", ["contact": .string(contact)])
    }

    public func chatHistory(chatId: Int64, limit: Int = 50) async throws -> JSONValue {
        try await request("chat.history", [
            "chat_id": .from(chatId),
            "limit": .from(limit)
        ])
    }

    public func chatSend(chatId: Int64, text: String) async throws -> JSONValue {
        // Поле протокола — "body" (docs/PROTOCOL.md); сервер читает именно его.
        try await request("chat.send", [
            "chat_id": .from(chatId),
            "body": .string(text)
        ])
    }

    // MARK: - Аура (агент)

    /// Запрос к Ауре. chatId=0 — без контекста чата (Siri/Shortcuts).
    public func agentAsk(chatId: Int64, message: String) async throws -> JSONValue {
        try await request("agent.ask", [
            "chat_id": .from(chatId),
            "message": .string(message)
        ])
    }

    /// Серверный STT: аудио в base64 (формат согласуется с AI-сервисом).
    public func speechTranscribe(audioBase64: String, language: String,
                                 format: String) async throws -> JSONValue {
        try await request("speech.transcribe", [
            "audio": .string(audioBase64),
            "language": .string(language),
            "format": .string(format)
        ])
    }

    // MARK: - Память и настройки

    public func memoryList() async throws -> JSONValue {
        try await request("memory.list")
    }

    public func prefsGet() async throws -> JSONValue {
        try await request("prefs.get")
    }

    public func prefsSet(_ preferences: JSONValue) async throws -> JSONValue {
        // Сервер читает поля настроек прямо из payload (docs/PROTOCOL.md):
        // prefs.set { "theme": ..., "notifications": {...} } — без обёртки.
        try await request("prefs.set", preferences)
    }

    // MARK: - Задачи (этап 8)

    public func tasksList(status: String = "") async throws -> JSONValue {
        var payload: [String: JSONValue] = [:]
        if !status.isEmpty { payload["status"] = .string(status) }
        return try await request("tasks.list", payload)
    }

    public func taskCreate(title: String, notes: String = "", remindAt: String = "") async throws -> JSONValue {
        var payload: [String: JSONValue] = ["title": .string(title)]
        if !notes.isEmpty { payload["notes"] = .string(notes) }
        if !remindAt.isEmpty { payload["remind_at"] = .string(remindAt) }
        return try await request("tasks.create", payload)
    }

    public func taskSetStatus(id: Int64, status: String) async throws -> JSONValue {
        try await request("tasks." + status, ["id": .from(id)])
    }

    public func taskDelete(id: Int64) async throws -> JSONValue {
        try await request("tasks.delete", ["id": .from(id)])
    }

    // MARK: - Разрешения и подтверждения (этап 8)

    public func permissionsList() async throws -> JSONValue {
        try await request("permissions.list")
    }

    public func permissionSet(tool: String, mode: String) async throws -> JSONValue {
        try await request("permissions.set", [
            "tool": .string(tool),
            "mode": .string(mode)
        ])
    }

    public func confirmationsList() async throws -> JSONValue {
        try await request("confirmation.list")
    }

    public func confirmationDecide(id: Int64, approve: Bool) async throws -> JSONValue {
        try await request(approve ? "confirmation.approve" : "confirmation.deny",
                          ["id": .from(id)])
    }

    // MARK: - Интеграции (этап 9)

    public func integrationsList() async throws -> JSONValue {
        try await request("integrations.list")
    }

    public func integrationBegin(provider: String, redirectUri: String) async throws -> JSONValue {
        try await request("integrations.begin", [
            "provider": .string(provider),
            "redirect_uri": .string(redirectUri)
        ])
    }

    public func integrationCallback(provider: String, code: String, state: String) async throws -> JSONValue {
        try await request("integrations.callback", [
            "provider": .string(provider),
            "code": .string(code),
            "state": .string(state)
        ])
    }

    public func integrationRevoke(id: Int64) async throws -> JSONValue {
        try await request("integrations.revoke", ["id": .from(id)])
    }

    public func integrationSync(id: Int64) async throws -> JSONValue {
        try await request("integrations.sync", ["id": .from(id)])
    }

    // MARK: - Уведомления и push-устройства (этап 13)

    public func notificationsList(unreadOnly: Bool = false, limit: Int = 50) async throws -> JSONValue {
        try await request("notifications.list", [
            "unread": .bool(unreadOnly),
            "limit": .from(Int64(limit))
        ])
    }

    /// id = 0 (или не передан) — отметить прочитанными все.
    public func notificationsRead(id: Int64 = 0) async throws -> JSONValue {
        try await request("notifications.read", ["id": .from(id)])
    }

    public func devicesPushRegister(platform: String, token: String) async throws -> JSONValue {
        try await request("devices.push.register", [
            "platform": .string(platform),
            "token": .string(token)
        ])
    }

    public func devicesPushList() async throws -> JSONValue {
        try await request("devices.push.list")
    }

    public func devicesPushRevoke(id: Int64) async throws -> JSONValue {
        try await request("devices.push.revoke", ["id": .from(id)])
    }

    // MARK: - Сервер

    public func serverInfo() async throws -> JSONValue {
        try await request("server.info")
    }
}
