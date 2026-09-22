// IntentSession.swift — общий WS-клиент для App Intents.
//
// Команды/Siri выполняются в процессе приложения, поэтому используют тот же
// Keychain (сервер + access-токен), что и UI. Соединение поднимается лениво
// и переиспользуется между запусками intent'ов.
import AuraKit
import Foundation

actor IntentSession {

    static let shared = IntentSession()

    private let client = AuraClient()
    private let keychain = KeychainStore()

    func ask(message: String) async throws -> String {
        try await ensureConnected()
        let payload = try await client.agentAsk(chatId: 0, message: message)
        let reply = payload.string("reply")
        return reply.isEmpty ? "Готово" : reply
    }

    func createTask(title: String, remindAt: String) async throws -> Int64 {
        try await ensureConnected()
        let payload = try await client.taskCreate(title: title, remindAt: remindAt)
        return payload.int("id")
    }

    private func ensureConnected() async throws {
        if client.isConnected { return }
        guard let serverText = keychain.load(.serverURL),
              let serverURL = URL(string: serverText) else {
            throw AuraError(code: "not_configured",
                            message: "Откройте Aura и укажите адрес сервера")
        }
        guard let token = keychain.load(.accessToken), !token.isEmpty else {
            throw AuraError(code: "unauthorized",
                            message: "Войдите в приложение Aura, чтобы команды работали")
        }
        try client.connect(serverURL: serverURL)
        _ = try await client.authenticate(token: token)
    }
}
