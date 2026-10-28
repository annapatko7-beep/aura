// DeepLink.swift — разбор ссылок схемы selfvoy:// (URL-схема приложения).
//
//   selfvoy://chats/{id}                        — открыть чат
//   selfvoy://voice                             — голосовой запрос (Back Tap → Команды)
//   selfvoy://ask?text=...                      — сразу спросить Selfvoy
//   selfvoy://tasks                             — список задач
//   selfvoy://settings                          — настройки
//   selfvoy://signup?ref=CODE                   — регистрация с кодом приглашения
//   selfvoy://oauth?provider=&code=&state=...   — возврат из Google OAuth
//
// В продакшене схема дополняется Universal Links (https) — тот же роутинг.
import Foundation

public enum DeepLink: Equatable, Sendable {
    case chats(id: Int64?)
    case voice
    case ask(text: String)
    case tasks
    /// Входящие уведомления (пуш о записи, оплате, «Приду» ведёт сюда).
    case notifications
    case settings
    /// Нормализованный реферальный код или "" (неверный код отбрасывается).
    case signup(ref: String)
    case oauth(provider: String, code: String, state: String)

    public static let scheme = "selfvoy"

    public init?(url: URL) {
        guard url.scheme?.lowercased() == DeepLink.scheme else { return nil }
        // URLComponents: host + компоненты пути и query.
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return nil
        }
        let host = components.host?.lowercased() ?? ""
        let queryItems = components.queryItems ?? []
        func query(_ name: String) -> String? {
            queryItems.first(where: { $0.name == name })?.value
        }

        switch host {
        case "chats":
            let id = components.path.split(separator: "/").first.flatMap { Int64($0) }
            self = .chats(id: id)
        case "voice":
            self = .voice
        case "ask":
            self = .ask(text: query("text") ?? "")
        case "tasks":
            self = .tasks
        case "notifications":
            self = .notifications
        case "settings":
            self = .settings
        case "signup":
            self = .signup(ref: ReferralCode.normalize(query("ref")) ?? "")
        case "oauth":
            guard let provider = query("provider"),
                  let code = query("code"),
                  let state = query("state") else { return nil }
            self = .oauth(provider: provider, code: code, state: state)
        default:
            return nil
        }
    }
}
