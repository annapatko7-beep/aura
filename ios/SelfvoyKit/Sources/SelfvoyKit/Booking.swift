// Booking.swift — страницы записи (booking.*) и рефералы (referral.*):
// модели ответа сервера (docs/PROTOCOL.md → «Страницы записи», «Цена, предоплата
// и политика отмены», «Пакеты», «Рефералы»).
// Зеркало Booking.kt (Android).
import Foundation

/// Предоплата страницы (F4): нет / вся сумма / депозит.
public enum BookingPrepay: String, CaseIterable, Sendable {
    case off = "none"
    case full
    case deposit

    public var title: String {
        switch self {
        case .off: return "Без предоплаты"
        case .full: return "Вся сумма"
        case .deposit: return "Депозит"
        }
    }
}

/// Поздняя отмена (позже `cancel_hours`): разрешена без возврата / запрещена.
public enum BookingLateCancel: String, CaseIterable, Sendable {
    case allow
    case block
}

/// Предложение пакета на странице (F5): `{id, title, sessions, price, valid_days}`; id 0 — новое.
public struct PackageOffer: Identifiable, Equatable, Hashable, Sendable {
    public static let titleMax = 80
    public static let maxPerPage = 5

    public var id: Int64
    public var title: String
    public var sessions: Int
    /// Цена в минимальных единицах валюты страницы.
    public var price: Int
    public var validDays: Int

    public init(id: Int64 = 0, title: String = "", sessions: Int = 4, price: Int = 0, validDays: Int = 90) {
        self.id = id
        self.title = title
        self.sessions = sessions
        self.price = price
        self.validDays = validDays
    }

    public init(json: JSONValue) {
        self.init(id: json.int("id"), title: json.string("title"), sessions: Int(json.int("sessions")),
                  price: Int(json.int("price")), validDays: Int(json.int("valid_days", default: 90)))
    }

    /// Ошибка для пользователя или nil — пределы сервера.
    public func validate() -> String? {
        let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty || name.unicodeScalars.count > Self.titleMax || name.contains("\n") {
            return "Название пакета — от 1 до \(Self.titleMax) символов в одну строку"
        }
        if !(2...100).contains(sessions) { return "Занятий в пакете — от 2 до 100" }
        if price <= 0 || price > Money.maxMinor { return "Укажите цену пакета" }
        if !(7...365).contains(validDays) { return "Срок действия пакета — от 7 до 365 дней" }
        return nil
    }

    /// Элемент массива `packages`: новый — без id.
    public var json: JSONValue {
        var object: [String: JSONValue] = [
            "title": .string(title.trimmingCharacters(in: .whitespacesAndNewlines)),
            "sessions": .from(sessions),
            "price": .from(price),
            "valid_days": .from(validDays),
        ]
        if id > 0 { object["id"] = .from(id) }
        return .object(object)
    }
}

/// Поля страницы для booking.pageCreate / pageUpdate; пределы — как на сервере.
public struct BookingPageFields: Equatable, Sendable {
    public static let titleMax = 120
    public static let descriptionMax = 1000
    public static let paymentUrlMax = 500
    public static let paymentNoteMax = 300
    public static let policyTextMax = 500

    public var title: String
    public var description: String
    public var durationMinutes: Int
    public var daysAhead: Int
    public var maxPerDay: Int
    public var noticeHours: Int
    public var maxSeries: Int
    public var weekends: Bool
    /// F1: напоминать гостю о встрече (за сутки и за 2 часа).
    public var guestReminders: Bool
    /// F3: скрыть подпись «Запись через Selfvoy» (Pro).
    public var hideBranding: Bool
    /// F4: цена в минимальных единицах (копейках); 0 — не показывать.
    public var price: Int
    public var currency: String
    public var prepay: BookingPrepay
    public var deposit: Int
    public var paymentUrl: String
    public var paymentNote: String
    public var payWithinMinutes: Int
    public var cancelHours: Int
    public var lateCancel: BookingLateCancel
    public var policyText: String
    /// F5: предложения пакетов (до 5; Pro).
    public var packages: [PackageOffer]

    public init(title: String = "", description: String = "", durationMinutes: Int = 30, daysAhead: Int = 14,
                maxPerDay: Int = 4, noticeHours: Int = 2, maxSeries: Int = 1, weekends: Bool = false,
                guestReminders: Bool = true, hideBranding: Bool = false, price: Int = 0,
                currency: String = "RUB", prepay: BookingPrepay = .off, deposit: Int = 0,
                paymentUrl: String = "", paymentNote: String = "", payWithinMinutes: Int = 60,
                cancelHours: Int = 0, lateCancel: BookingLateCancel = .allow, policyText: String = "",
                packages: [PackageOffer] = []) {
        self.title = title
        self.description = description
        self.durationMinutes = durationMinutes
        self.daysAhead = daysAhead
        self.maxPerDay = maxPerDay
        self.noticeHours = noticeHours
        self.maxSeries = maxSeries
        self.weekends = weekends
        self.guestReminders = guestReminders
        self.hideBranding = hideBranding
        self.price = price
        self.currency = currency
        self.prepay = prepay
        self.deposit = deposit
        self.paymentUrl = paymentUrl
        self.paymentNote = paymentNote
        self.payWithinMinutes = payWithinMinutes
        self.cancelHours = cancelHours
        self.lateCancel = lateCancel
        self.policyText = policyText
        self.packages = packages
    }

    /// Из объекта страницы (`page`); старый сервер без полей F1–F5 — значения по умолчанию.
    public init(json: JSONValue) {
        self.init(
            title: json.string("title"), description: json.string("description"),
            durationMinutes: Int(json.int("duration_minutes", default: 30)),
            daysAhead: Int(json.int("days_ahead", default: 14)),
            maxPerDay: Int(json.int("max_per_day", default: 4)),
            noticeHours: Int(json.int("notice_hours", default: 2)),
            maxSeries: Int(json.int("max_series", default: 1)), weekends: json.bool("weekends"),
            guestReminders: json.bool("guest_reminders", default: true),
            hideBranding: json.bool("hide_branding"),
            price: Int(json.int("price")),
            currency: json.string("currency", default: "RUB").isEmpty ? "RUB" : json.string("currency", default: "RUB"),
            prepay: BookingPrepay(rawValue: json.string("prepay")) ?? .off,
            deposit: Int(json.int("deposit")),
            paymentUrl: json.string("payment_url"), paymentNote: json.string("payment_note"),
            payWithinMinutes: Int(json.int("pay_within_minutes", default: 60)),
            cancelHours: Int(json.int("cancel_hours")),
            lateCancel: BookingLateCancel(rawValue: json.string("late_cancel")) ?? .allow,
            policyText: json.string("policy_text"),
            packages: json.array("packages").map(PackageOffer.init(json:)))
    }

    private static func singleLine(_ text: String, max: Int) -> Bool {
        text.unicodeScalars.count <= max && !text.contains("\n") && !text.contains("\r")
    }

    /// Ошибка для пользователя или nil — те же пределы, что проверяет сервер.
    /// `paymentsConnected == false` — ЮKassa не подключена: для предоплаты нужны ссылка или реквизиты.
    public func validate(paymentsConnected: Bool? = nil) -> String? {
        let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty { return "Назовите страницу — например, «Консультация»" }
        if name.unicodeScalars.count > Self.titleMax || name.contains("\n") {
            return "Название — до \(Self.titleMax) символов в одну строку"
        }
        if description.trimmingCharacters(in: .whitespacesAndNewlines).unicodeScalars.count > Self.descriptionMax {
            return "Описание — до \(Self.descriptionMax) символов"
        }
        if !(15...240).contains(durationMinutes) { return "Длительность — от 15 до 240 минут" }
        if !(1...60).contains(daysAhead) { return "Запись вперёд — от 1 до 60 дней" }
        if !(1...16).contains(maxPerDay) { return "Встреч в день — от 1 до 16" }
        if !(0...168).contains(noticeHours) { return "Запас до встречи — от 0 до 168 часов" }
        if !(1...12).contains(maxSeries) { return "Серия — от 1 до 12 встреч" }
        if !(0...Money.maxMinor).contains(price) { return "Цена — от 0 до 1 000 000" }
        if !Money.currencies.contains(where: { $0.code == currency }) { return "Валюта — RUB, USD, EUR, KZT или BYN" }
        let url = paymentUrl.trimmingCharacters(in: .whitespacesAndNewlines)
        if !url.isEmpty && !Self.validPaymentUrl(url) {
            return "Ссылка оплаты — адрес https:// не длиннее \(Self.paymentUrlMax) символов"
        }
        let note = paymentNote.trimmingCharacters(in: .whitespacesAndNewlines)
        if !Self.singleLine(note, max: Self.paymentNoteMax) {
            return "Реквизиты — до \(Self.paymentNoteMax) символов в одну строку"
        }
        if !(15...1440).contains(payWithinMinutes) { return "Срок оплаты — от 15 минут до суток" }
        if !(0...168).contains(cancelHours) { return "Бесплатная отмена — от 0 до 168 часов" }
        if policyText.trimmingCharacters(in: .whitespacesAndNewlines).unicodeScalars.count > Self.policyTextMax {
            return "Условия отмены — до \(Self.policyTextMax) символов"
        }
        if prepay != .off {
            if price <= 0 { return "Для предоплаты укажите цену" }
            if prepay == .deposit && !(1...price).contains(deposit) { return "Депозит — больше нуля и не больше цены" }
            if paymentsConnected == false && url.isEmpty && note.isEmpty {
                return "Для предоплаты нужен способ оплаты: ссылка, реквизиты или ЮKassa"
            }
        }
        if packages.count > PackageOffer.maxPerPage { return "Не больше \(PackageOffer.maxPerPage) пакетов на странице" }
        for offer in packages {
            if let problem = offer.validate() { return problem }
        }
        return nil
    }

    /// Ссылка оплаты: только https, без пробелов и кавычек, ≤ 500 символов (как на сервере).
    public static func validPaymentUrl(_ url: String) -> Bool {
        guard url.utf8.count <= paymentUrlMax, url.utf8.count > 8, url.hasPrefix("https://") else { return false }
        return !url.unicodeScalars.contains { $0.value <= 0x20 || $0.value == 0x7F || "\"<>\\".unicodeScalars.contains($0) }
    }

    /// Payload booking.pageCreate / pageUpdate (без page_id) — все поля.
    public var payload: [String: JSONValue] {
        [
            "title": .string(title.trimmingCharacters(in: .whitespacesAndNewlines)),
            "description": .string(description.trimmingCharacters(in: .whitespacesAndNewlines)),
            "duration_minutes": .from(durationMinutes),
            "days_ahead": .from(daysAhead),
            "max_per_day": .from(maxPerDay),
            "notice_hours": .from(noticeHours),
            "max_series": .from(maxSeries),
            "weekends": .from(weekends),
            "guest_reminders": .from(guestReminders),
            "hide_branding": .from(hideBranding),
            "price": .from(price),
            "currency": .string(currency),
            "prepay": .string(prepay.rawValue),
            "deposit": .from(prepay == .deposit ? deposit : 0),
            "payment_url": .string(paymentUrl.trimmingCharacters(in: .whitespacesAndNewlines)),
            "payment_note": .string(paymentNote.trimmingCharacters(in: .whitespacesAndNewlines)),
            "pay_within_minutes": .from(payWithinMinutes),
            "cancel_hours": .from(cancelHours),
            "late_cancel": .string(lateCancel.rawValue),
            "policy_text": .string(policyText.trimmingCharacters(in: .whitespacesAndNewlines)),
            "packages": .array(packages.map(\.json)),
        ]
    }

    /// Payload booking.pageUpdate — только изменённые поля (сервер меняет переданные;
    /// способ оплаты проверяется, лишь когда трогают поля оплаты). Пусто — менять нечего.
    public func changes(from original: BookingPageFields) -> [String: JSONValue] {
        let now = payload
        let before = original.payload
        return now.filter { key, value in before[key] != value }
    }
}

public struct BookingPage: Identifiable, Equatable, Sendable {
    public var id: Int64
    public var slug: String
    /// Готовая ссылка или "" — если на сервере не задан SELFVOY_PUBLIC_URL.
    public var url: String
    public var fields: BookingPageFields
    /// active | paused
    public var status: String
    public var upcoming: Int
    public var createdAt: String

    /// Ссылка для гостя: от сервера или путь веб-клиента.
    public var link: String { url.isEmpty ? "#/b/\(slug)" : url }
    public var isActive: Bool { status == "active" }

    public init(json: JSONValue) {
        id = json.int("id")
        slug = json.string("slug")
        url = json.string("url")
        fields = BookingPageFields(json: json)
        status = json.string("status")
        upcoming = Int(json.int("upcoming"))
        createdAt = json.string("created_at")
    }
}

public struct BookingOccurrence: Equatable, Hashable, Sendable {
    public var start: String
    public var end: String
    public var status: String
    /// Местное время владельца «YYYY-MM-DDTHH:MM» (у старых записей — пусто).
    public var local: String
    /// F1: когда гость нажал «Приду» ("" — не подтверждал).
    public var confirmedAt: String
    public var active: Bool { status == "active" }
    public var confirmed: Bool { !confirmedAt.isEmpty }

    public init(start: String, end: String, status: String, local: String = "", confirmedAt: String = "") {
        self.start = start
        self.end = end
        self.status = status
        self.local = local
        self.confirmedAt = confirmedAt
    }

    public init(json: JSONValue) {
        self.init(start: json.string("start"), end: json.string("end"), status: json.string("status"),
                  local: json.string("local"), confirmedAt: json.string("confirmed_at"))
    }
}

/// F1: напоминание гостю о встрече `start` — `24h` или `2h`; `sentAt` "" — ещё не отправлено.
public struct BookingReminder: Equatable, Hashable, Sendable {
    public var start: String
    public var kind: String
    public var dueAt: String
    public var sentAt: String

    public init(json: JSONValue) {
        start = json.string("start")
        kind = json.string("kind")
        dueAt = json.string("due_at")
        sentAt = json.string("sent_at")
    }
}

/// Запись гостя глазами владельца.
public struct Booking: Identifiable, Equatable, Sendable {
    public var id: Int64
    public var pageId: Int64
    public var pageTitle: String
    public var guestName: String
    public var guestEmail: String
    public var guestComment: String
    /// F5: второй получатель извещений ("" — нет).
    public var ccEmail: String
    /// F5: пакет, по которому запись (0 — без пакета).
    public var packageId: Int64
    /// active | cancelled
    public var status: String
    /// "" | guest | owner | system (оплата не поступила)
    public var cancelledBy: String
    public var reschedules: Int
    public var occurrences: [BookingOccurrence]
    public var reminders: [BookingReminder]
    /// F4: оплата записи или nil.
    public var payment: BookingPayment?
    public var createdAt: String
    public var updatedAt: String
    /// Цена услуги, зафиксированная при записи (минимальные единицы): 0 — занятие по
    /// абонементу или бесплатная услуга; nil — старый сервер без поля (берём цену страницы).
    /// Не путать с `payment.amount` — это сумма оплаты онлайн (депозит или предоплата).
    public var price: Int?
    /// Валюта `price`; "" — старый сервер.
    public var currency: String

    public var isActive: Bool { status == "active" }

    /// Занятие по абонементу: деньги взяты при покупке пакета.
    public var byPackage: Bool { packageId > 0 }

    /// Стоимость встречи: цена записи; старый сервер — цена страницы; по абонементу — 0.
    public func price(with page: BookingPage?) -> Int {
        if byPackage { return 0 }
        if let price { return max(0, price) }
        return max(0, page?.fields.price ?? 0)
    }

    /// Валюта стоимости встречи (запись → страница → RUB).
    public func currency(with page: BookingPage?) -> String {
        if !currency.isEmpty { return currency }
        if let code = page?.fields.currency, !code.isEmpty { return code }
        return "RUB"
    }

    public init(json: JSONValue) {
        id = json.int("id")
        pageId = json.int("page_id")
        pageTitle = json.string("page_title")
        guestName = json.string("guest_name")
        guestEmail = json.string("guest_email")
        guestComment = json.string("guest_comment")
        ccEmail = json.string("cc_email")
        packageId = json.int("package_id")
        status = json.string("status")
        cancelledBy = json.string("cancelled_by")
        reschedules = Int(json.int("reschedules"))
        occurrences = json.array("occurrences").map(BookingOccurrence.init(json:)).sorted { $0.start < $1.start }
        reminders = json.array("reminders").map(BookingReminder.init(json:))
        payment = BookingPayment(json: json["payment"])
        createdAt = json.string("created_at")
        updatedAt = json.string("updated_at")
        price = json["price"]?.intValue.map { Int($0) }
        currency = json.string("currency")
    }
}

public struct Referral: Hashable, Sendable {
    public var displayName: String
    public var source: String
    public var joinedAt: String
    public var verified: Bool
}

public struct ReferralInfo: Equatable, Sendable {
    public var code: String
    /// Готовая ссылка или "" — если на сервере не задан SELFVOY_PUBLIC_URL.
    public var url: String
    public var joined: Int
    public var verified: Int
    /// F3: сколько наград (месяц Pro) начислено пригласившему (≤ rewardCap).
    public var rewarded: Int
    public var rewardDays: Int
    public var rewardCap: Int
    public var plan: SelfvoyPlan?
    public var referrals: [Referral]

    /// Сколько ещё приглашённых принесут награду.
    public var rewardsLeft: Int { max(0, rewardCap - rewarded) }

    /// «+30 дней Pro за каждого — начислено 2 из 12».
    public var rewardText: String {
        let days = rewardDays > 0 ? rewardDays : 30
        if rewardCap > 0 && rewarded >= rewardCap {
            return "Все \(rewardCap) наград получены — спасибо, что зовёте коллег"
        }
        return "Месяц Pro (\(days) дн.) вам и коллеге за каждого, кто подтвердит email · начислено \(rewarded) из \(rewardCap)"
    }

    /// Есть ли полная ссылка; без SELFVOY_PUBLIC_URL делимся только кодом.
    public var hasLink: Bool { !url.isEmpty }

    /// Что показать и скопировать: полная ссылка или (без SELFVOY_PUBLIC_URL) сам код —
    /// голый путь «#/signup?ref=…» без адреса веб-клиента никуда не ведёт.
    public var link: String { url.isEmpty ? code : url }

    /// Текст для «Поделиться»: ссылка или код, который друг введёт при регистрации.
    public var shareText: String { hasLink ? "Заведи свой Selfvoy: \(url)" : "Код приглашения в Selfvoy: \(code)" }

    public init(json: JSONValue) {
        code = json.string("code")
        url = json.string("url")
        joined = Int(json.int("joined"))
        verified = Int(json.int("verified"))
        rewarded = Int(json.int("rewarded"))
        rewardDays = Int(json.int("reward_days", default: 30))
        rewardCap = Int(json.int("reward_cap", default: 12))
        plan = SelfvoyPlan(json: json["plan"])
        referrals = json.array("referrals").map {
            Referral(displayName: $0.string("display_name"), source: $0.string("source"),
                     joinedAt: $0.string("joined_at"), verified: $0.bool("verified"))
        }
    }
}

/// Реферальный код: 8 символов A–Z/0–9 (сервер сравнивает в верхнем регистре).
public enum ReferralCode {
    public static let length = 8
    private static let allowed = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")

    /// Нормализованный код или nil — неверный код не отправляем (сервер его всё равно молча проигнорирует).
    public static func normalize(_ raw: String?) -> String? {
        guard let raw else { return nil }
        let code = raw.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        guard code.unicodeScalars.count == length,
              code.unicodeScalars.allSatisfy({ allowed.contains($0) })
        else { return nil }
        return code
    }
}
