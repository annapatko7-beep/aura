// JSONValue.swift — динамический JSON для payloads протокола Aura.
//
// Сервер отвечает разнородными объектами (payload у каждого типа свой),
// поэтому вместо десятков Codable-структур используется единое дерево
// с точечными хелперами (string/int/bool/array). Кодируется обратно
// без потерь — нужно для отправки payload-запросов.
import Foundation

public enum JSONValue: Codable, Equatable, Sendable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let value = try? container.decode(Bool.self) {
            self = .bool(value)
        } else if let value = try? container.decode(Double.self) {
            self = .number(value)
        } else if let value = try? container.decode(String.self) {
            self = .string(value)
        } else if let value = try? container.decode([JSONValue].self) {
            self = .array(value)
        } else if let value = try? container.decode([String: JSONValue].self) {
            self = .object(value)
        } else {
            throw DecodingError.dataCorruptedError(
                in: container, debugDescription: "неизвестный тип JSON")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        }
    }

    // MARK: - Хелперы доступа

    public var stringValue: String? {
        if case .string(let value) = self { return value }
        return nil
    }

    public var intValue: Int64? {
        if case .number(let value) = self { return Int64(value) }
        if case .string(let value) = self { return Int64(value) }
        return nil
    }

    public var doubleValue: Double? {
        if case .number(let value) = self { return value }
        return nil
    }

    public var boolValue: Bool? {
        if case .bool(let value) = self { return value }
        return nil
    }

    public var arrayValue: [JSONValue] {
        if case .array(let value) = self { return value }
        return []
    }

    public var objectValue: [String: JSONValue] {
        if case .object(let value) = self { return value }
        return [:]
    }

    public subscript(key: String) -> JSONValue? {
        get {
            if case .object(let value) = self { return value[key] }
            return nil
        }
        set {
            // Запись допустима только в объект; прочие варианты не изменяем.
            guard case .object(var value) = self else { return }
            value[key] = newValue
            self = .object(value)
        }
    }

    public func string(_ key: String, default fallback: String = "") -> String {
        self[key]?.stringValue ?? fallback
    }

    public func int(_ key: String, default fallback: Int64 = 0) -> Int64 {
        self[key]?.intValue ?? fallback
    }

    public func bool(_ key: String, default fallback: Bool = false) -> Bool {
        self[key]?.boolValue ?? fallback
    }

    public func array(_ key: String) -> [JSONValue] {
        self[key]?.arrayValue ?? []
    }

    // MARK: - Конструкторы литералов

    public static func from(_ value: String) -> JSONValue { .string(value) }
    public static func from(_ value: Int) -> JSONValue { .number(Double(value)) }
    public static func from(_ value: Int64) -> JSONValue { .number(Double(value)) }
    public static func from(_ value: Bool) -> JSONValue { .bool(value) }

    public static var emptyObject: JSONValue { .object([:]) }
}
