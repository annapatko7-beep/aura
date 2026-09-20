// KeychainStore.swift — хранение секретов в Keychain (не в UserDefaults).
//
// Держит access/refresh-токены, адрес сервера и идентификатор устройства.
// Тот же принцип, что у Qt-клиента, но токены — только в защищённом хранилище.
import Foundation
import Security

public struct KeychainStore: Sendable {

    public enum Key: String, Sendable {
        case accessToken = "aura.access_token"
        case refreshToken = "aura.refresh_token"
        case serverURL = "aura.server_url"
        case deviceId = "aura.device_id"
    }

    private let service: String

    public init(service: String = "ai.aura.app") {
        self.service = service
    }

    public func save(_ value: String, for key: Key) {
        guard let data = value.data(using: .utf8) else { return }
        var query = baseQuery(for: key)
        SecItemDelete(query as CFDictionary)
        query[kSecValueData as String] = data
        query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(query as CFDictionary, nil)
    }

    public func load(_ key: Key) -> String? {
        var query = baseQuery(for: key)
        query[kSecReturnData as String] = kCFBooleanTrue
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    public func delete(_ key: Key) {
        SecItemDelete(baseQuery(for: key) as CFDictionary)
    }

    public func clearAll() {
        for key in [Key.accessToken, .refreshToken, .serverURL] {
            delete(key)
        }
    }

    private func baseQuery(for key: Key) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: key.rawValue
        ]
    }
}
