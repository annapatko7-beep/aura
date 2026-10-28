// Growth.swift — функции роста F1–F6 на стороне владельца (docs/PROTOCOL.md):
// деньги и оплата записи, пакеты (абонементы), пост «свободные окна», ICS-подписка
// на календарь, Telegram-бот, ЮKassa, код `pro_required` и виды уведомлений.
// Зеркало web/src/lib/{money,payment,integrations}.ts.
import Foundation

// MARK: - Даты

/// Разбор ISO-дат сервера и русские подписи (без зависимости от UI-слоя).
public enum SelfvoyDate {
    private static let isoPlain: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        return formatter
    }()

    private static let isoFractional: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    public static func parse(_ text: String) -> Date? {
        let value = text.trimmingCharacters(in: .whitespaces)
        guard !value.isEmpty else { return nil }
        return isoPlain.date(from: value) ?? isoFractional.date(from: value)
    }

    private static func formatter(_ pattern: String, _ zone: TimeZone) -> DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "ru_RU")
        formatter.timeZone = zone
        formatter.dateFormat = pattern
        return formatter
    }

    /// «14:05».
    public static func time(_ date: Date, zone: TimeZone = .current) -> String {
        formatter("HH:mm", zone).string(from: date)
    }

    /// «12 ноября».
    public static func dayMonth(_ date: Date, zone: TimeZone = .current) -> String {
        formatter("d MMMM", zone).string(from: date)
    }

    /// «пт» — короткий день недели строчными.
    public static func weekdayShort(_ date: Date, zone: TimeZone = .current) -> String {
        formatter("EEEEEE", zone).string(from: date).lowercased()
    }

    /// «сегодня, 14:05» / «завтра, 9:30» / «вчера, 18:40» / «пт, 23 октября, 10:00».
    public static func when(_ iso: String, now: Date = Date(), calendar: Calendar = .current) -> String {
        guard let date = parse(iso) else { return "" }
        let zone = calendar.timeZone
        let clock = time(date, zone: zone)
        if calendar.isDate(date, inSameDayAs: now) { return "сегодня, \(clock)" }
        if let tomorrow = calendar.date(byAdding: .day, value: 1, to: now),
           calendar.isDate(date, inSameDayAs: tomorrow) { return "завтра, \(clock)" }
        if let yesterday = calendar.date(byAdding: .day, value: -1, to: now),
           calendar.isDate(date, inSameDayAs: yesterday) { return "вчера, \(clock)" }
        return "\(weekdayShort(date, zone: zone)), \(dayMonth(date, zone: zone)), \(clock)"
    }

    public static func plural(_ n: Int, _ one: String, _ few: String, _ many: String) -> String {
        let m10 = abs(n) % 10
        let m100 = abs(n) % 100
        if m10 == 1 && m100 != 11 { return one }
        if (2...4).contains(m10) && (m100 < 12 || m100 > 14) { return few }
        return many
    }
}

// MARK: - Деньги

/// Суммы на сервере — целые минимальные единицы (копейки, центы).
public enum Money {
    public struct Currency: Equatable, Sendable {
        public let code: String
        public let title: String
        public let symbol: String
    }

    public static let currencies: [Currency] = [
        Currency(code: "RUB", title: "Рубли, ₽", symbol: "₽"),
        Currency(code: "USD", title: "Доллары, $", symbol: "$"),
        Currency(code: "EUR", title: "Евро, €", symbol: "€"),
        Currency(code: "KZT", title: "Тенге, ₸", symbol: "₸"),
        Currency(code: "BYN", title: "Белорусские рубли, Br", symbol: "Br"),
    ]

    /// Наибольшая цена на сервере: 100 000 000 минимальных единиц.
    public static let maxMinor = 100_000_000

    /// Неразрывный пробел: сумма не переносится по строкам.
    static let nbsp = "\u{00A0}"

    public static func symbol(_ currency: String) -> String {
        currencies.first { $0.code == currency }?.symbol ?? (currency.isEmpty ? "₽" : currency)
    }

    private static func groups(_ value: Int) -> String {
        let digits = Array(String(value))
        var out = ""
        for (index, digit) in digits.enumerated() {
            if index > 0 && (digits.count - index) % 3 == 0 { out += nbsp }
            out.append(digit)
        }
        return out
    }

    /// «2 000 ₽», «2 000,50 ₽», «15 $»; копейки — только если они есть.
    public static func format(_ minor: Int, currency: String = "RUB") -> String {
        let negative = minor < 0
        let value = Swift.abs(minor)
        let cents = value % 100
        var body = groups(value / 100)
        if cents > 0 { body += "," + (cents < 10 ? "0\(cents)" : "\(cents)") }
        return (negative ? "−" : "") + body + nbsp + symbol(currency)
    }

    /// Ввод суммы («2 000,5», «1500.00 ₽») → минимальные единицы; пусто → 0; мусор → nil.
    public static func parse(_ text: String) -> Int? {
        var value = text.filter { !$0.isWhitespace }
        for mark in ["₽", "$", "€", "₸", "Br"] { value = value.replacingOccurrences(of: mark, with: "") }
        if value.isEmpty { return 0 }
        let parts = value.replacingOccurrences(of: ".", with: ",").split(separator: ",", omittingEmptySubsequences: false)
        guard parts.count <= 2, let wholeText = parts.first, !wholeText.isEmpty,
              wholeText.allSatisfy({ $0.isASCII && $0.isNumber }), wholeText.count <= 12,
              let whole = Int(wholeText) else { return nil }
        var cents = 0
        if parts.count == 2 {
            let fraction = parts[1]
            guard fraction.count <= 2, fraction.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
            cents = Int(String(fraction).padding(toLength: 2, withPad: "0", startingAt: 0)) ?? 0
        }
        return whole * 100 + cents
    }

    /// Сумма для поля ввода: 200000 → «2000», 200050 → «2000,50», 0 → "".
    public static func inputValue(_ minor: Int) -> String {
        guard minor > 0 else { return "" }
        let cents = minor % 100
        return cents > 0 ? "\(minor / 100),\(cents < 10 ? "0\(cents)" : "\(cents)")" : "\(minor / 100)"
    }
}

// MARK: - Оплата записи и пакета (F4)

/// Значок статуса: экран сопоставляет его с SF Symbol. Статус — всегда значок и слово.
public enum StatusIcon: String, Sendable {
    case check, clock, cross, undo, minus, hand, plus, bell, calendar
}

/// Оттенок статуса (не единственный носитель смысла — рядом всегда слово).
public enum StatusTone: String, Sendable {
    case green, amber, red, accent, attention, neutral
}

public struct StatusView: Equatable, Sendable {
    public let label: String
    public let tone: StatusTone
    public let icon: StatusIcon

    public init(_ label: String, _ tone: StatusTone, _ icon: StatusIcon) {
        self.label = label
        self.tone = tone
        self.icon = icon
    }
}

/// Отметка оплаты владельцем: booking.paymentSet / package.paymentSet.
public enum OwnerPaymentMark: String, CaseIterable, Sendable {
    case paid
    case waived
    case refunded

    public var title: String {
        switch self {
        case .paid: return "Отметить оплату"
        case .waived: return "Без оплаты"
        case .refunded: return "Возврат"
        }
    }
}

/// `payment`: `{status, amount, currency, kind, method, action_url, due_at, paid_at, refunded_at}`.
public struct BookingPayment: Equatable, Hashable, Sendable {
    /// pending | claimed | paid | waived | refunded | expired
    public var status: String
    public var amount: Int
    public var currency: String
    /// full | deposit | package
    public var kind: String
    /// link | yookassa
    public var method: String
    public var actionUrl: String
    public var dueAt: String
    public var paidAt: String
    public var refundedAt: String

    public init(status: String, amount: Int = 0, currency: String = "RUB", kind: String = "full",
                method: String = "link", actionUrl: String = "", dueAt: String = "", paidAt: String = "",
                refundedAt: String = "") {
        self.status = status
        self.amount = amount
        self.currency = currency
        self.kind = kind
        self.method = method
        self.actionUrl = actionUrl
        self.dueAt = dueAt
        self.paidAt = paidAt
        self.refundedAt = refundedAt
    }

    /// Нет объекта (null или поле отсутствует) — nil: оплаты нет.
    public init?(json: JSONValue?) {
        guard let json, case .object = json else { return nil }
        self.init(status: json.string("status"), amount: Int(json.int("amount")),
                  currency: json.string("currency", default: "RUB"), kind: json.string("kind"),
                  method: json.string("method"), actionUrl: json.string("action_url"),
                  dueAt: json.string("due_at"), paidAt: json.string("paid_at"),
                  refundedAt: json.string("refunded_at"))
    }

    public var amountText: String { Money.format(amount, currency: currency) }

    /// Ждёт ли денег от гостя.
    public var awaiting: Bool { status == "pending" || status == "claimed" }

    /// Подпись для владельца: что сделать.
    public var ownerStatus: StatusView? { Self.ownerStatus(status) }

    public static func ownerStatus(_ status: String) -> StatusView? {
        switch status {
        case "pending": return StatusView("Ждём оплату", .amber, .clock)
        case "claimed": return StatusView("Проверьте оплату", .amber, .hand)
        case "paid": return StatusView("Оплачено", .green, .check)
        case "refunded": return StatusView("Возврат", .neutral, .undo)
        case "waived": return StatusView("Без оплаты", .neutral, .minus)
        case "expired": return StatusView("Не оплачено", .red, .cross)
        default: return nil
        }
    }

    /// Что может отметить владелец: ждём → «оплачено»/«без оплаты»; оплачено → «возврат».
    public var ownerActions: [OwnerPaymentMark] {
        switch status {
        case "pending", "claimed": return [.paid, .waived]
        case "paid": return [.refunded]
        case "waived", "expired": return [.paid]
        default: return []
        }
    }
}

// MARK: - Пакеты и абонементы (F5)

/// Пакет глазами владельца (`package.list`, `package.issue`, …).
public struct OwnerPackage: Identifiable, Equatable, Sendable {
    public var id: Int64
    public var pageId: Int64
    public var pageTitle: String
    /// 0 — выдан вне предложений страницы.
    public var offerId: Int64
    public var title: String
    public var sessionsTotal: Int
    public var sessionsUsed: Int
    public var sessionsLeft: Int
    public var validDays: Int
    /// "" — ещё не активирован (ждёт оплату).
    public var validUntil: String
    public var price: Int
    public var currency: String
    /// pending | active | used | expired | cancelled
    public var status: String
    public var payment: BookingPayment?
    public var guestName: String
    public var guestEmail: String
    public var ccEmail: String
    public var createdAt: String

    public init(json: JSONValue) {
        id = json.int("id")
        pageId = json.int("page_id")
        pageTitle = json.string("page_title")
        offerId = json.int("offer_id")
        title = json.string("title")
        sessionsTotal = Int(json.int("sessions_total"))
        sessionsUsed = Int(json.int("sessions_used"))
        sessionsLeft = Int(json.int("sessions_left"))
        validDays = Int(json.int("valid_days", default: 90))
        validUntil = json.string("valid_until")
        price = Int(json.int("price"))
        currency = json.string("currency", default: "RUB")
        status = json.string("status")
        payment = BookingPayment(json: json["payment"])
        guestName = json.string("guest_name")
        guestEmail = json.string("guest_email")
        ccEmail = json.string("cc_email")
        createdAt = json.string("created_at")
    }

    public static let statuses = ["pending", "active", "used", "expired", "cancelled"]

    /// Статус пакета словом и значком.
    public var statusView: StatusView {
        switch status {
        case "pending": return StatusView("Ждёт оплату", .amber, .clock)
        case "active":
            return sessionsLeft <= 1 ? StatusView("Заканчивается", .attention, .bell)
                                     : StatusView("Активен", .green, .check)
        case "used": return StatusView("Занятия закончились", .neutral, .minus)
        case "expired": return StatusView("Срок истёк", .neutral, .clock)
        case "cancelled": return StatusView("Отменён", .red, .cross)
        default: return StatusView(status, .neutral, .minus)
        }
    }

    /// «осталось 3 из 8».
    public var remainingText: String { "осталось \(sessionsLeft) из \(sessionsTotal)" }

    /// Доля использованного — для полоски прогресса.
    public var usedFraction: Double {
        sessionsTotal > 0 ? min(1, max(0, Double(sessionsTotal - sessionsLeft) / Double(sessionsTotal))) : 0
    }

    /// Можно ли ещё менять остаток и отменять.
    public var isOpen: Bool { status != "cancelled" }
}

/// Запрос `package.issue`: продажа пакета вне сайта.
public struct PackageIssueRequest: Equatable, Sendable {
    public var pageId: Int64
    /// Предложение страницы; nil — свой пакет (title + sessions + price + validDays).
    public var offerId: Int64?
    public var title: String
    public var sessions: Int
    public var price: Int
    public var validDays: Int
    public var guestName: String
    public var guestEmail: String
    public var ccEmail: String
    public var paid: Bool

    public init(pageId: Int64, offerId: Int64? = nil, title: String = "", sessions: Int = 4, price: Int = 0,
                validDays: Int = 90, guestName: String = "", guestEmail: String = "", ccEmail: String = "",
                paid: Bool = false) {
        self.pageId = pageId
        self.offerId = offerId
        self.title = title
        self.sessions = sessions
        self.price = price
        self.validDays = validDays
        self.guestName = guestName
        self.guestEmail = guestEmail
        self.ccEmail = ccEmail
        self.paid = paid
    }

    private static func looksLikeEmail(_ text: String) -> Bool {
        let parts = text.split(separator: "@", omittingEmptySubsequences: false)
        return parts.count == 2 && !parts[0].isEmpty && parts[1].contains(".") && !text.contains(" ")
            && text.utf8.count <= 254
    }

    public func validate() -> String? {
        if pageId <= 0 { return "Выберите страницу записи" }
        let name = guestName.trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty || name.unicodeScalars.count > 80 { return "Имя клиента — от 1 до 80 символов" }
        let email = guestEmail.trimmingCharacters(in: .whitespacesAndNewlines)
        let cc = ccEmail.trimmingCharacters(in: .whitespacesAndNewlines)
        if !email.isEmpty && !Self.looksLikeEmail(email) { return "Проверьте email клиента" }
        if !cc.isEmpty && !Self.looksLikeEmail(cc) { return "Проверьте email второго получателя" }
        if !cc.isEmpty && cc.lowercased() == email.lowercased() { return "Второй получатель совпадает с клиентом" }
        if offerId == nil {
            let name = title.trimmingCharacters(in: .whitespacesAndNewlines)
            if name.isEmpty || name.unicodeScalars.count > PackageOffer.titleMax || name.contains("\n") {
                return "Название пакета — от 1 до \(PackageOffer.titleMax) символов"
            }
            if !(1...100).contains(sessions) { return "Занятий — от 1 до 100" }
            if !(0...Money.maxMinor).contains(price) { return "Проверьте цену" }
            if !(7...365).contains(validDays) { return "Срок действия — от 7 до 365 дней" }
        }
        return nil
    }

    public var payload: [String: JSONValue] {
        var payload: [String: JSONValue] = [
            "page_id": .from(pageId),
            "guest_name": .string(guestName.trimmingCharacters(in: .whitespacesAndNewlines)),
            "paid": .from(paid),
        ]
        if let offerId {
            payload["offer_id"] = .from(offerId)
        } else {
            payload["title"] = .string(title.trimmingCharacters(in: .whitespacesAndNewlines))
            payload["sessions"] = .from(sessions)
            payload["price"] = .from(price)
            payload["valid_days"] = .from(validDays)
        }
        let email = guestEmail.trimmingCharacters(in: .whitespacesAndNewlines)
        if !email.isEmpty { payload["guest_email"] = .string(email) }
        let cc = ccEmail.trimmingCharacters(in: .whitespacesAndNewlines)
        if !cc.isEmpty { payload["cc_email"] = .string(cc) }
        return payload
    }
}

/// Ответ `package.issue`: токен ссылки пакета показывается **один раз**.
public struct PackageIssueResult: Equatable, Sendable {
    public var package: OwnerPackage
    public var token: String
    /// `<SELFVOY_PUBLIC_URL>/#/p/<токен>` или "".
    public var url: String

    public init(json: JSONValue) {
        package = OwnerPackage(json: json["package"] ?? .emptyObject)
        token = json.string("token")
        url = json.string("url")
    }

    /// Что отдать клиенту: полная ссылка или путь веб-клиента.
    public var link: String { url.isEmpty ? "#/p/\(token)" : url }
}

// MARK: - Пост «свободные окна» (F6)

public enum SlotsPostFormat: String, CaseIterable, Sendable {
    case telegram
    case vk
    case plain

    public var title: String {
        switch self {
        case .telegram: return "Telegram"
        case .vk: return "VK"
        case .plain: return "Текст"
        }
    }
}

/// Ответ `booking.slotsPost`: `{text, slots, url}`.
public struct SlotsPost: Equatable, Sendable {
    public var text: String
    public var slots: Int
    public var url: String

    public init(json: JSONValue) {
        text = json.string("text")
        slots = Int(json.int("slots"))
        url = json.string("url")
    }

    public init(text: String, slots: Int, url: String) {
        self.text = text
        self.slots = slots
        self.url = url
    }

    /// Текст для «Поделиться»: без SELFVOY_PUBLIC_URL сервер не добавляет ссылку —
    /// дописываем известную клиенту (если есть).
    public func shareText(fallbackLink: String? = nil) -> String {
        guard url.isEmpty, let link = fallbackLink, !link.isEmpty, !text.contains(link) else { return text }
        return text + "\n\nЗаписаться: " + link
    }
}

// MARK: - Календарь: ICS-подписка (F6)

/// `calendar.feed*`: `{enabled, url, path}`; ссылка видна только сразу после `feedReset`.
public struct CalendarFeed: Equatable, Sendable {
    public var enabled: Bool
    public var url: String
    public var path: String

    public init(enabled: Bool = false, url: String = "", path: String = "") {
        self.enabled = enabled
        self.url = url
        self.path = path
    }

    public init(json: JSONValue) {
        self.init(enabled: json.bool("enabled"), url: json.string("url"), path: json.string("path"))
    }

    /// Адрес подписки: готовый `url` сервера или адрес сервера (wss→https, ws→http) + `path`.
    public func resolvedURL(serverURL: String) -> String? {
        let ready = url.trimmingCharacters(in: .whitespaces)
        if ready.lowercased().hasPrefix("https://") || ready.lowercased().hasPrefix("http://") { return ready }
        let tail = path.trimmingCharacters(in: .whitespaces)
        guard tail.hasPrefix("/"),
              let server = URLComponents(string: serverURL.trimmingCharacters(in: .whitespaces)),
              let host = server.host, !host.isEmpty else { return nil }
        let scheme: String
        switch server.scheme?.lowercased() {
        case "wss": scheme = "https"
        case "ws": scheme = "http"
        default: return nil
        }
        let port = server.port.map { ":\($0)" } ?? ""
        let hostPart = host.contains(":") ? "[\(host)]" : host
        return "\(scheme)://\(hostPart)\(port)\(tail)"
    }

    /// webcal:// — календарь iOS открывает подписку сразу (только для https).
    public static func webcal(_ url: String?) -> String? {
        guard let url, url.lowercased().hasPrefix("https://") else { return nil }
        return "webcal://" + String(url.dropFirst("https://".count))
    }
}

// MARK: - Telegram-бот (F2)

/// `telegram.status`: `{enabled, bot_username, linked, username}`.
public struct TelegramStatus: Equatable, Sendable {
    public var enabled: Bool
    public var botUsername: String
    public var linked: Bool
    public var username: String

    public init(enabled: Bool = false, botUsername: String = "", linked: Bool = false, username: String = "") {
        self.enabled = enabled
        self.botUsername = botUsername
        self.linked = linked
        self.username = username
    }

    public init(json: JSONValue) {
        self.init(enabled: json.bool("enabled"), botUsername: json.string("bot_username"),
                  linked: json.bool("linked"), username: json.string("username"))
    }

    public var summary: String {
        if !enabled { return "Бот не настроен на сервере" }
        if linked { return username.isEmpty ? "Подключено" : "Подключено · @\(username)" }
        return "Не подключено"
    }
}

/// `telegram.link`: ссылка на бота на 15 минут.
public struct TelegramLink: Equatable, Sendable {
    public var url: String
    public var expiresAt: String

    public init(json: JSONValue) {
        url = json.string("url")
        expiresAt = json.string("expires_at")
    }
}

// MARK: - ЮKassa (F4)

/// `payments.status`: `{connected, provider, shop_id, connected_at}`.
public struct PaymentsStatus: Equatable, Sendable {
    public var connected: Bool
    public var provider: String
    public var shopId: String
    public var connectedAt: String

    public init(connected: Bool = false, provider: String = "", shopId: String = "", connectedAt: String = "") {
        self.connected = connected
        self.provider = provider
        self.shopId = shopId
        self.connectedAt = connectedAt
    }

    public init(json: JSONValue) {
        // shop_id может прийти числом или строкой.
        let raw = json["shop_id"]
        let shop = raw?.stringValue ?? raw?.intValue.map { String($0) } ?? ""
        self.init(connected: json.bool("connected"), provider: json.string("provider"), shopId: shop,
                  connectedAt: json.string("connected_at"))
    }
}

/// Проверка ключей ЮKassa до отправки. Секретный ключ клиент **не хранит и не логирует**:
/// он живёт только в поле ввода и в одном запросе `payments.connect`.
public enum YooKassaCredentials {
    public static func validate(shopId: String, secretKey: String) -> String? {
        let shop = shopId.trimmingCharacters(in: .whitespaces)
        if shop.isEmpty || shop.count > 20 || !shop.allSatisfy({ $0.isASCII && $0.isNumber }) {
            return "shopId — только цифры, до 20 знаков"
        }
        let secret = secretKey.trimmingCharacters(in: .whitespaces)
        if !(secret.hasPrefix("live_") || secret.hasPrefix("test_")) || secret.count < 8
            || secret.contains(where: { $0.isWhitespace }) {
            return "Секретный ключ начинается с live_ или test_"
        }
        return nil
    }

    public static func payload(shopId: String, secretKey: String) -> [String: JSONValue] {
        [
            "provider": .string("yookassa"),
            "shop_id": .string(shopId.trimmingCharacters(in: .whitespaces)),
            "secret_key": .string(secretKey.trimmingCharacters(in: .whitespaces)),
        ]
    }
}

// MARK: - Тариф: pro_required

public enum ProRequired {
    public static let code = "pro_required"
    public static let text = "Это доступно на тарифе Pro. Пригласите коллегу в Selfvoy — вы оба получите месяц Pro (Ещё → Пригласить коллегу)."
}

extension SelfvoyError {
    /// Действие требует Pro (сервер с SELFVOY_PLANS_ENFORCED=1, Pro не действует).
    public var isProRequired: Bool { code == ProRequired.code }
}

// MARK: - Сервер

/// `server.info`: то, что нужно клиенту (Telegram-бот; "" — не настроен).
public struct ServerInfo: Equatable, Sendable {
    public var telegramBot: String

    public init(json: JSONValue) {
        telegramBot = json.string("telegram_bot")
    }
}

// MARK: - Виды уведомлений

/// Значения `notifications[].kind` (docs/PROTOCOL.md → «Уведомления и push»).
public enum SelfvoyNotificationKind {
    public static let taskDue = "task.due"
    public static let confirmationRequested = "confirmation.requested"
    public static let a2aProposal = "a2a.proposal"
    public static let loginNew = "login.new"
    public static let bookingCreated = "booking.created"
    public static let bookingRescheduled = "booking.rescheduled"
    public static let bookingCancelled = "booking.cancelled"
    public static let bookingConfirmed = "booking.confirmed"
    public static let bookingPayment = "booking.payment"
    public static let packagePurchased = "package.purchased"
    public static let packagePayment = "package.payment"
    public static let packageLow = "package.low"
    public static let referralJoined = "referral.joined"
    public static let referralReward = "referral.reward"

    /// Уведомления, после которых стоит перечитать записи владельца.
    public static func touchesBookings(_ kind: String) -> Bool {
        kind.hasPrefix("booking.") || kind == packagePurchased
    }

    /// После которых стоит перечитать пакеты.
    public static func touchesPackages(_ kind: String) -> Bool {
        kind.hasPrefix("package.")
    }

    /// После которых стоит перечитать тариф и рефералы.
    public static func touchesPlan(_ kind: String) -> Bool {
        kind == referralReward || kind == referralJoined
    }

    /// Статус-значок уведомления (значок + слово рядом в заголовке).
    public static func status(_ kind: String, payload: JSONValue = .emptyObject) -> StatusView {
        switch kind {
        case bookingCreated: return StatusView("Новая запись", .attention, .plus)
        case bookingRescheduled: return StatusView("Перенос", .accent, .calendar)
        case bookingCancelled: return StatusView("Отмена", .red, .cross)
        case bookingConfirmed: return StatusView("Придёт", .green, .check)
        case bookingPayment, packagePayment:
            switch payload.string("status") {
            case "paid": return StatusView("Оплачено", .green, .check)
            case "claimed": return StatusView("Проверьте оплату", .amber, .hand)
            case "refunded": return StatusView("Возврат", .neutral, .undo)
            case "refund_due": return StatusView("Верните оплату", .red, .undo)
            case "expired": return StatusView("Не оплачено", .red, .cross)
            default: return StatusView("Оплата", .amber, .clock)
            }
        case packagePurchased: return StatusView("Пакет куплен", .attention, .plus)
        case packageLow: return StatusView("Пакет заканчивается", .amber, .bell)
        case referralJoined: return StatusView("Новый коллега", .accent, .plus)
        case referralReward: return StatusView("Месяц Pro", .green, .check)
        case confirmationRequested: return StatusView("Нужно решение", .amber, .hand)
        case taskDue: return StatusView("Напоминание", .accent, .bell)
        case loginNew: return StatusView("Вход", .amber, .bell)
        default: return StatusView("Уведомление", .neutral, .bell)
        }
    }
}
