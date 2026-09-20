// Protocol.swift — кадры WS-протокола Aura (те же, что у C++/Qt-клиентов).
//
// Запрос клиента:  {"id": "...", "type": "auth.login", "payload": {...}}
// Ответ сервера:   {"id": "...", "type": "ok",    "payload": {...}}
//                  {"id": "...", "type": "error", "code": "...", "message": "..."}
// Событие сервера: {"type": "event", "event": "chat.message", "payload": {...}}
import Foundation

public struct AuraRequestFrame: Encodable {
    public let id: String
    public let type: String
    public let payload: JSONValue

    public init(id: String, type: String, payload: JSONValue) {
        self.id = id
        self.type = type
        self.payload = payload
    }
}

public struct AuraResponseFrame: Decodable {
    public let id: String?
    public let type: String?
    public let code: String?
    public let message: String?
    public let event: String?
    public let payload: JSONValue?
}

/// Ошибка протокола: сервер вернул кадр {"type":"error", code, message}.
public struct AuraError: Error, LocalizedError, Equatable, Sendable {
    public let code: String
    public let message: String

    public init(code: String, message: String) {
        self.code = code
        self.message = message
    }

    public var errorDescription: String? { message }
}

/// Имена событий сервера (push-уведомления внутри WS-сессии).
public enum AuraEventName {
    public static let chatMessage = "chat.message"
    public static let taskDue = "task.due"
}
