// DeepLink.swift — разбор ссылок схемы aura:// (URL-схема приложения).
//
//   aura://chats/{id}                        — открыть чат
//   aura://voice                             — голосовой запрос (Back Tap → Команды)
//   aura://ask?text=...                      — сразу спросить Ауру
//   aura://tasks                             — список задач
//   aura://settings                          — настройки
//   aura://oauth?provider=&code=&state=...   — возврат из Google OAuth
//
// В продакшене схема дополняется Universal Links (https) — тот же роутинг.
import Foundation

public enum DeepLink: Equatable, Sendable {
    case chats(id: Int64?)
    case voice
    case ask(text: String)
    case tasks
    case settings
    case oauth(provider: String, code: String, state: String)

    public static let scheme = "aura"

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
        case "settings":
            self = .settings
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
