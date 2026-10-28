// Models.swift — типизированные модели протокола там, где форма стабильна:
// пользователь (auth.me, profile.update) и приватность переговоров (prefs.privacy).
//
// Все поля декодируются мягко (decodeIfPresent + значение по умолчанию):
// старый сервер без bio/privacy не ломает клиента.
import Foundation

// MARK: - Пользователь

/// Пользователь из `auth.me` / `auth.login` / `profile.update` (docs/PROTOCOL.md).
public struct SelfvoyUser: Codable, Equatable, Sendable {
    public var id: Int64
    public var email: String
    public var displayName: String
    /// «О себе» (до 300 символов); у старых payload поля нет — пустая строка.
    public var bio: String
    public var avatarUrl: String
    /// Тариф (F3); у старого сервера поля нет — nil.
    public var plan: SelfvoyPlan?

    enum CodingKeys: String, CodingKey {
        case id
        case email
        case displayName = "display_name"
        case bio
        case avatarUrl = "avatar_url"
        case plan
    }

    public init(id: Int64 = 0, email: String = "", displayName: String = "",
                bio: String = "", avatarUrl: String = "", plan: SelfvoyPlan? = nil) {
        self.id = id
        self.email = email
        self.displayName = displayName
        self.bio = bio
        self.avatarUrl = avatarUrl
        self.plan = plan
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decodeIfPresent(Int64.self, forKey: .id) ?? 0
        email = try container.decodeIfPresent(String.self, forKey: .email) ?? ""
        displayName = try container.decodeIfPresent(String.self, forKey: .displayName) ?? ""
        bio = try container.decodeIfPresent(String.self, forKey: .bio) ?? ""
        avatarUrl = try container.decodeIfPresent(String.self, forKey: .avatarUrl) ?? ""
        plan = try container.decodeIfPresent(SelfvoyPlan.self, forKey: .plan)
    }

    /// Из динамического payload (`auth.me` отдаёт пользователя в корне, прочие — в `user`).
    public init(json: JSONValue) {
        self.init(id: json.int("id"), email: json.string("email"),
                  displayName: json.string("display_name"), bio: json.string("bio"),
                  avatarUrl: json.string("avatar_url"),
                  plan: SelfvoyPlan(json: json["plan"]))
    }
}

// MARK: - Тариф

/// `user.plan`: `{name: pro|free, until: ISO|"", enforced}` (docs/PROTOCOL.md → «Авторизация»).
/// `name` честный: Pro, пока `until` в будущем; `enforced` — включены ли ограничения Free.
public struct SelfvoyPlan: Codable, Equatable, Sendable {
    public var name: String
    public var until: String
    public var enforced: Bool

    public init(name: String = "free", until: String = "", enforced: Bool = false) {
        self.name = name
        self.until = until
        self.enforced = enforced
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        name = try container.decodeIfPresent(String.self, forKey: .name) ?? "free"
        until = try container.decodeIfPresent(String.self, forKey: .until) ?? ""
        enforced = try container.decodeIfPresent(Bool.self, forKey: .enforced) ?? false
    }

    /// Из объекта `plan`; нет объекта (старый сервер) — nil.
    public init?(json: JSONValue?) {
        guard let json, case .object = json else { return nil }
        self.init(name: json.string("name", default: "free"), until: json.string("until"),
                  enforced: json.bool("enforced"))
    }

    /// Действует ли Pro сейчас: `name == pro` и срок не прошёл (пустой срок — бессрочно).
    public func isPro(now: Date = Date()) -> Bool {
        guard name == "pro" else { return false }
        guard let date = SelfvoyDate.parse(until) else { return true }
        return date > now
    }

    /// Доступны ли функции Pro: действует Pro или идёт бета (ограничения выключены).
    public func proFeaturesOpen(now: Date = Date()) -> Bool {
        isPro(now: now) || !enforced
    }

    /// Подпись бейджа: «Pro до 12 ноября» / «Pro» / «Free».
    public func badge(now: Date = Date()) -> String {
        guard isPro(now: now) else { return "Free" }
        guard let date = SelfvoyDate.parse(until) else { return "Pro" }
        return "Pro до \(SelfvoyDate.dayMonth(date))"
    }

    /// Пояснение под бейджем.
    public func hint(now: Date = Date()) -> String {
        if isPro(now: now) { return "Скрытая подпись, предоплата, пакеты и ЮKassa доступны" }
        return enforced ? "Функции Pro: скрыть подпись, предоплата, пакеты, ЮKassa"
                        : "Пока идёт бета, функции Pro открыты всем"
    }
}

/// Ограничения `profile.update` — те же, что проверяет сервер.
public enum SelfvoyProfileRules {
    public static let displayNameMax = 80
    public static let bioMax = 300

    /// Текст ошибки для пользователя или nil, если имя и «о себе» допустимы.
    /// Пробелы по краям не считаются (сервер их обрезает).
    public static func validate(displayName: String, bio: String) -> String? {
        let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty { return "Введите имя" }
        if name.count > displayNameMax { return "Имя — не длиннее \(displayNameMax) символов" }
        if bio.trimmingCharacters(in: .whitespacesAndNewlines).count > bioMax {
            return "«О себе» — не длиннее \(bioMax) символов"
        }
        return nil
    }
}

// MARK: - Приватность переговоров

/// `prefs.privacy`: что Selfvoy раскрывает чужому Selfvoy в переговорах.
/// Все флаги по умолчанию `true` (так же отдаёт сервер).
public struct SelfvoyPrivacy: Codable, Equatable, Sendable {

    public enum Flag: String, CaseIterable, Sendable {
        case shareFreeSlotsOnly = "share_free_slots_only"
        case allowPlaceChoice = "allow_place_choice"
        case hideEventTitles = "hide_event_titles"
    }

    public var shareFreeSlotsOnly: Bool
    public var hideEventTitles: Bool
    public var allowPlaceChoice: Bool

    enum CodingKeys: String, CodingKey {
        case shareFreeSlotsOnly = "share_free_slots_only"
        case hideEventTitles = "hide_event_titles"
        case allowPlaceChoice = "allow_place_choice"
    }

    public init(shareFreeSlotsOnly: Bool = true, hideEventTitles: Bool = true,
                allowPlaceChoice: Bool = true) {
        self.shareFreeSlotsOnly = shareFreeSlotsOnly
        self.hideEventTitles = hideEventTitles
        self.allowPlaceChoice = allowPlaceChoice
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        shareFreeSlotsOnly = try container.decodeIfPresent(Bool.self, forKey: .shareFreeSlotsOnly) ?? true
        hideEventTitles = try container.decodeIfPresent(Bool.self, forKey: .hideEventTitles) ?? true
        allowPlaceChoice = try container.decodeIfPresent(Bool.self, forKey: .allowPlaceChoice) ?? true
    }

    /// Из объекта `privacy` (нет объекта или ключа — значение по умолчанию `true`).
    public init(json: JSONValue?) {
        let value = json ?? .emptyObject
        self.init(shareFreeSlotsOnly: value.bool(Flag.shareFreeSlotsOnly.rawValue, default: true),
                  hideEventTitles: value.bool(Flag.hideEventTitles.rawValue, default: true),
                  allowPlaceChoice: value.bool(Flag.allowPlaceChoice.rawValue, default: true))
    }

    /// Из ответа `prefs.get` / `prefs.set` (настройки целиком).
    public init(preferences: JSONValue) {
        self.init(json: preferences["privacy"])
    }

    public subscript(flag: Flag) -> Bool {
        get {
            switch flag {
            case .shareFreeSlotsOnly: return shareFreeSlotsOnly
            case .hideEventTitles: return hideEventTitles
            case .allowPlaceChoice: return allowPlaceChoice
            }
        }
        set {
            switch flag {
            case .shareFreeSlotsOnly: shareFreeSlotsOnly = newValue
            case .hideEventTitles: hideEventTitles = newValue
            case .allowPlaceChoice: allowPlaceChoice = newValue
            }
        }
    }

    /// Все три флага как JSON-объект.
    public var json: JSONValue {
        .object(Dictionary(uniqueKeysWithValues: Flag.allCases.map { ($0.rawValue, JSONValue.bool(self[$0])) }))
    }
}
