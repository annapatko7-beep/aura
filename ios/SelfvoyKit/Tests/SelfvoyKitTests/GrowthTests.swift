// GrowthTests.swift — функции роста F1–F6 на стороне владельца: тариф, поля страницы
// (напоминания, подпись, цена, предоплата, политика, пакеты), оплата записи, пакеты,
// лента «Сегодня», история записи, Telegram, ЮKassa, ICS-подписка, пост «свободные окна».
import XCTest

@testable import SelfvoyKit

private func decode(_ text: String) throws -> JSONValue {
    try JSONDecoder().decode(JSONValue.self, from: Data(text.utf8))
}

private var utc: Calendar {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = TimeZone(identifier: "UTC")!
    return calendar
}

private func date(_ iso: String) -> Date { SelfvoyDate.parse(iso)! }

final class PlanTests: XCTestCase {

    func testUserCarriesPlan() throws {
        let user = SelfvoyUser(json: try decode("""
        {"id": 1, "email": "a@b.c", "plan": {"name": "pro", "until": "2030-06-15T12:00:00Z", "enforced": true}}
        """))
        XCTAssertEqual(user.plan?.name, "pro")
        XCTAssertEqual(user.plan?.enforced, true)
        let now = date("2030-01-01T12:00:00Z")
        XCTAssertTrue(user.plan!.isPro(now: now))
        XCTAssertEqual(user.plan!.badge(now: now), "Pro до 15 июня")
        XCTAssertFalse(user.plan!.isPro(now: date("2031-01-01T00:00:00Z")))
        XCTAssertEqual(user.plan!.badge(now: date("2031-01-01T00:00:00Z")), "Free")
        // Codable-путь тоже читает plan; старый payload без plan — nil.
        let decoded = try JSONDecoder().decode(SelfvoyUser.self, from: Data("""
        {"id": 2, "plan": {"name": "free", "until": "", "enforced": false}}
        """.utf8))
        XCTAssertEqual(decoded.plan, SelfvoyPlan(name: "free", until: "", enforced: false))
        XCTAssertNil(SelfvoyUser(json: try decode("{\"id\": 3}")).plan)
    }

    func testFreePlanBetaAndEnforced() {
        let beta = SelfvoyPlan(name: "free", until: "", enforced: false)
        XCTAssertFalse(beta.isPro())
        XCTAssertTrue(beta.proFeaturesOpen())
        XCTAssertEqual(beta.badge(), "Free")
        XCTAssertTrue(beta.hint().contains("бета"))
        let enforced = SelfvoyPlan(name: "free", until: "", enforced: true)
        XCTAssertFalse(enforced.proFeaturesOpen())
        XCTAssertEqual(SelfvoyPlan(name: "pro").badge(), "Pro")
    }

    func testProRequiredError() {
        XCTAssertTrue(SelfvoyError(code: "pro_required", message: "x").isProRequired)
        XCTAssertFalse(SelfvoyError(code: "forbidden", message: "x").isProRequired)
        XCTAssertTrue(ProRequired.text.contains("месяц Pro"))
    }

    func testReferralReward() throws {
        let info = ReferralInfo(json: try decode("""
        {"code": "ABCD2345", "url": "https://s.example/#/signup?ref=ABCD2345", "joined": 3, "verified": 2,
         "rewarded": 2, "reward_days": 30, "reward_cap": 12,
         "plan": {"name": "pro", "until": "2030-01-01T00:00:00Z", "enforced": false}, "referrals": []}
        """))
        XCTAssertEqual(info.rewarded, 2)
        XCTAssertEqual(info.rewardsLeft, 10)
        XCTAssertEqual(info.plan?.name, "pro")
        XCTAssertTrue(info.rewardText.contains("начислено 2 из 12"))
        var capped = info
        capped.rewarded = 12
        XCTAssertTrue(capped.rewardText.hasPrefix("Все 12"))
        // Старый сервер: награды по умолчанию 30 дней и 12 наград.
        let old = ReferralInfo(json: try decode("{\"code\": \"ABCD2345\"}"))
        XCTAssertEqual(old.rewardDays, 30)
        XCTAssertEqual(old.rewardCap, 12)
        XCTAssertNil(old.plan)
    }
}

final class PageFieldsTests: XCTestCase {

    func testDecodeAllGrowthFields() throws {
        let page = BookingPage(json: try decode("""
        {"id": 5, "slug": "abcdefghij", "url": "https://s.example/#/b/abcdefghij", "title": "Урок",
         "duration_minutes": 60, "days_ahead": 14, "max_per_day": 4, "notice_hours": 2, "max_series": 1,
         "weekends": false, "status": "active", "upcoming": 3, "guest_reminders": false, "hide_branding": true,
         "price": 200000, "currency": "EUR", "prepay": "deposit", "deposit": 50000,
         "payment_url": "https://pay.example/x", "payment_note": "СБП +7…", "pay_within_minutes": 120,
         "cancel_hours": 12, "late_cancel": "block", "policy_text": "Без возврата позже",
         "packages": [{"id": 1, "title": "8 уроков", "sessions": 8, "price": 1400000, "valid_days": 60}]}
        """))
        let f = page.fields
        XCTAssertFalse(f.guestReminders)
        XCTAssertTrue(f.hideBranding)
        XCTAssertEqual(f.price, 200000)
        XCTAssertEqual(f.currency, "EUR")
        XCTAssertEqual(f.prepay, .deposit)
        XCTAssertEqual(f.deposit, 50000)
        XCTAssertEqual(f.payWithinMinutes, 120)
        XCTAssertEqual(f.cancelHours, 12)
        XCTAssertEqual(f.lateCancel, .block)
        XCTAssertEqual(f.packages, [PackageOffer(id: 1, title: "8 уроков", sessions: 8, price: 1_400_000, validDays: 60)])
        XCTAssertNil(f.validate())
        XCTAssertTrue(page.isActive)
    }

    func testValidatePaymentRules() {
        var f = BookingPageFields(title: "Урок", prepay: .full)
        XCTAssertEqual(f.validate(), "Для предоплаты укажите цену")
        f.price = 100_000
        XCTAssertNil(f.validate())
        XCTAssertNotNil(f.validate(paymentsConnected: false))
        f.paymentNote = "СБП по номеру"
        XCTAssertNil(f.validate(paymentsConnected: false))
        f.prepay = .deposit
        XCTAssertNotNil(f.validate())
        f.deposit = 200_000
        XCTAssertNotNil(f.validate(), "депозит больше цены")
        f.deposit = 30_000
        XCTAssertNil(f.validate())
        f.paymentUrl = "http://pay.example"
        XCTAssertNotNil(f.validate(), "только https")
        f.paymentUrl = "https://pay.example/a b"
        XCTAssertNotNil(f.validate(), "без пробелов")
        f.paymentUrl = "https://pay.example/ok"
        XCTAssertNil(f.validate())
        f.paymentNote = "строка\nвторая"
        XCTAssertNotNil(f.validate())
        f.paymentNote = ""
        f.payWithinMinutes = 10
        XCTAssertNotNil(f.validate())
        f.payWithinMinutes = 60
        f.cancelHours = 200
        XCTAssertNotNil(f.validate())
        f.cancelHours = 24
        f.currency = "GBP"
        XCTAssertNotNil(f.validate())
    }

    func testPackagesRules() {
        var f = BookingPageFields(title: "Урок", packages: [PackageOffer(title: "Пакет", sessions: 1, price: 100)])
        XCTAssertEqual(f.validate(), "Занятий в пакете — от 2 до 100")
        f.packages = [PackageOffer(title: "Пакет", sessions: 4, price: 0)]
        XCTAssertNotNil(f.validate())
        f.packages = Array(repeating: PackageOffer(title: "П", sessions: 4, price: 100), count: 6)
        XCTAssertNotNil(f.validate())
        f.packages = [PackageOffer(title: "Новый", sessions: 4, price: 100), PackageOffer(id: 3, title: "Старый", sessions: 8, price: 200)]
        XCTAssertNil(f.validate())
        let list = f.payload["packages"]?.arrayValue ?? []
        XCTAssertNil(list[0]["id"], "новый пакет — без id")
        XCTAssertEqual(list[1]["id"], .number(3))
        XCTAssertEqual(list[0]["valid_days"], .number(90))
    }

    func testChangesSendOnlyEditedFields() {
        let original = BookingPageFields(title: "Урок", price: 100_000, paymentNote: "СБП")
        var edited = original
        edited.title = "Урок математики"
        edited.guestReminders = false
        let changes = edited.changes(from: original)
        XCTAssertEqual(Set(changes.keys), ["title", "guest_reminders"])
        XCTAssertEqual(changes["guest_reminders"], .bool(false))
        XCTAssertTrue(original.changes(from: original).isEmpty)
        // Депозит уходит только при prepay = deposit.
        var deposit = original
        deposit.deposit = 5000
        XCTAssertTrue(deposit.changes(from: original).isEmpty)
        deposit.prepay = .deposit
        XCTAssertEqual(Set(deposit.changes(from: original).keys), ["prepay", "deposit"])
    }
}

final class MoneyTests: XCTestCase {

    func testFormat() {
        XCTAssertEqual(Money.format(200_000), "2\u{00A0}000\u{00A0}₽")
        XCTAssertEqual(Money.format(200_050), "2\u{00A0}000,50\u{00A0}₽")
        XCTAssertEqual(Money.format(1505, currency: "USD"), "15,05\u{00A0}$")
        XCTAssertEqual(Money.format(123_456_700), "1\u{00A0}234\u{00A0}567\u{00A0}₽")
        XCTAssertEqual(Money.format(0, currency: "BYN"), "0\u{00A0}Br")
        XCTAssertEqual(Money.format(-500), "−5\u{00A0}₽")
    }

    func testParseAndInput() {
        XCTAssertEqual(Money.parse(""), 0)
        XCTAssertEqual(Money.parse("2 000"), 200_000)
        XCTAssertEqual(Money.parse("2000,5"), 200_050)
        XCTAssertEqual(Money.parse("1500.00 ₽"), 150_000)
        XCTAssertNil(Money.parse("12,345"))
        XCTAssertNil(Money.parse("-5"))
        XCTAssertNil(Money.parse("abc"))
        XCTAssertNil(Money.parse("1,2,3"))
        XCTAssertEqual(Money.inputValue(200_000), "2000")
        XCTAssertEqual(Money.inputValue(200_050), "2000,50")
        XCTAssertEqual(Money.inputValue(200_005), "2000,05")
        XCTAssertEqual(Money.inputValue(0), "")
    }
}

final class BookingPaymentTests: XCTestCase {

    private func booking(_ extra: String) throws -> Booking {
        Booking(json: try decode("""
        {"id": 7, "page_id": 2, "page_title": "Урок", "guest_name": "Мария", "guest_email": "m@x.ru",
         "cc_email": "mama@x.ru", "package_id": 4, "status": "active", "cancelled_by": "", "reschedules": 0,
         "created_at": "2026-10-20T10:00:00Z", "updated_at": "2026-10-20T10:00:00Z",
         "occurrences": [{"start": "2026-10-22T12:30:00Z", "end": "2026-10-22T13:30:00Z", "status": "active",
                          "local": "2026-10-22T15:30", "confirmed_at": ""}],
         "reminders": [{"start": "2026-10-22T12:30:00Z", "kind": "2h", "due_at": "2026-10-22T10:30:00Z", "sent_at": ""},
                       {"start": "2026-10-22T12:30:00Z", "kind": "24h", "due_at": "2026-10-21T12:30:00Z",
                        "sent_at": "2026-10-21T12:31:00Z"}]\(extra)}
        """))
    }

    func testParsePaymentAndActions() throws {
        let b = try booking("""
        , "payment": {"status": "pending", "amount": 200000, "currency": "RUB", "kind": "deposit",
            "method": "link", "action_url": "https://pay", "due_at": "2026-10-20T11:00:00Z", "paid_at": "", "refunded_at": ""}
        """)
        XCTAssertEqual(b.ccEmail, "mama@x.ru")
        XCTAssertEqual(b.packageId, 4)
        XCTAssertEqual(b.reminders.count, 2)
        XCTAssertEqual(b.payment?.amountText, "2\u{00A0}000\u{00A0}₽")
        XCTAssertEqual(b.payment?.ownerActions, [.paid, .waived])
        XCTAssertEqual(b.payment?.ownerStatus?.label, "Ждём оплату")
        XCTAssertTrue(b.payment!.awaiting)
        XCTAssertEqual(BookingPayment(status: "paid").ownerActions, [.refunded])
        XCTAssertEqual(BookingPayment(status: "expired").ownerActions, [.paid])
        XCTAssertEqual(BookingPayment(status: "refunded").ownerActions, [])
        XCTAssertNil(try booking(", \"payment\": null").payment)
        XCTAssertNil(try booking("").payment)
        XCTAssertEqual(SelfvoyClient.paymentSetPayload(key: "booking_id", id: 7, status: .waived),
                       ["booking_id": .number(7), "status": .string("waived")])
    }

    func testHistorySteps() throws {
        let b = try booking("""
        , "payment": {"status": "claimed", "amount": 200000, "currency": "RUB", "kind": "full",
            "method": "link", "action_url": "", "due_at": "", "paid_at": "", "refunded_at": ""}
        """)
        let now = date("2026-10-22T09:00:00Z")
        let steps = OwnerToday.history(b, now: now, calendar: utc)
        XCTAssertEqual(steps.map(\.id), ["created", "payment", "rem-24h", "rem-2h", "confirmed"])
        XCTAssertEqual(steps[1].title, "Гость отметил: оплатил(а) 2\u{00A0}000\u{00A0}₽")
        XCTAssertEqual(steps[2].title, "Напоминание за сутки отправлено")
        XCTAssertEqual(steps[2].detail, "вчера, 12:31")
        XCTAssertEqual(steps[3].detail, "запланировано на сегодня, 10:30")
        XCTAssertEqual(steps[4].title, "Ещё не подтвердил(а)")

        var cancelled = b
        cancelled.status = "cancelled"
        cancelled.cancelledBy = "system"
        XCTAssertEqual(OwnerToday.history(cancelled, now: now, calendar: utc).last?.title,
                       "Запись отменена: оплата не поступила")
    }

    func testOccurrenceStatusPriority() throws {
        let now = date("2026-10-22T09:00:00Z")
        var b = try booking("")
        // Новая запись (меньше суток) — «внимание».
        XCTAssertEqual(OwnerToday.occurrenceStatus(b, b.occurrences[0], now: date("2026-10-20T20:00:00Z")).label, "Новая запись")
        XCTAssertEqual(OwnerToday.occurrenceStatus(b, b.occurrences[0], now: now).label, "Записан(а)")
        b.occurrences[0].confirmedAt = "2026-10-21T13:00:00Z"
        XCTAssertEqual(OwnerToday.occurrenceStatus(b, b.occurrences[0], now: now).label, "Подтвердил(а)")
        b.payment = BookingPayment(status: "pending", amount: 100)
        XCTAssertEqual(OwnerToday.occurrenceStatus(b, b.occurrences[0], now: now).label, "Ждём оплату")
        b.status = "cancelled"
        XCTAssertEqual(OwnerToday.occurrenceStatus(b, b.occurrences[0], now: now).icon, .cross)
    }
}

final class TodayTests: XCTestCase {

    private func page(_ id: Int64, price: Int = 0) throws -> BookingPage {
        BookingPage(json: try decode("""
        {"id": \(id), "slug": "s\(id)", "title": "Стр \(id)", "price": \(price), "currency": "RUB", "status": "active"}
        """))
    }

    private func booking(_ id: Int64, page: Int64, start: String, end: String, status: String = "active") throws -> Booking {
        Booking(json: try decode("""
        {"id": \(id), "page_id": \(page), "guest_name": "Гость \(id)", "status": "\(status)",
         "created_at": "2026-10-01T00:00:00Z",
         "occurrences": [{"start": "\(start)", "end": "\(end)", "status": "active"}]}
        """))
    }

    func testBuildDayOrdersAndFilters() throws {
        let pages = [try page(2, price: 200_000), try page(9, price: 150_000)]
        let bookings = [
            try booking(1, page: 2, start: "2026-10-22T10:00:00Z", end: "2026-10-22T11:00:00Z"),
            try booking(2, page: 9, start: "2026-10-22T15:00:00Z", end: "2026-10-22T16:00:00Z"),
            try booking(3, page: 2, start: "2026-10-22T12:00:00Z", end: "2026-10-22T13:00:00Z", status: "cancelled"),
            try booking(4, page: 2, start: "2026-10-23T10:00:00Z", end: "2026-10-23T11:00:00Z"),
        ]
        let tasks = [
            DayTaskInput(id: 1, title: "Купить мел", at: date("2026-10-22T09:00:00Z")),
            // Задача, созданная записью (due_at = start встречи), не дублируется.
            DayTaskInput(id: 2, title: "Встреча", at: date("2026-10-22T15:00:00Z")),
        ]
        let free = [
            FreeSlot(start: "2026-10-22T11:30:00Z", end: "2026-10-22T12:30:00Z", pageId: 2),
            FreeSlot(start: "2026-10-22T15:30:00Z", end: "2026-10-22T16:30:00Z", pageId: 2), // пересекается
            FreeSlot(start: "2026-10-22T08:00:00Z", end: "2026-10-22T09:00:00Z", pageId: 2), // уже прошло
        ]
        let now = date("2026-10-22T10:30:00Z")
        let entries = OwnerToday.buildDay(bookings: bookings, tasks: tasks, free: free, pages: pages,
                                          day: now, now: now, calendar: utc)
        XCTAssertEqual(entries.map(\.id), ["t1", "b1-2026-10-22T10:00:00Z", "now",
                                           "f2-\(Int(date("2026-10-22T11:30:00Z").timeIntervalSince1970))",
                                           "b2-2026-10-22T15:00:00Z"])
        if case .booking(_, _, _, _, let color, _) = entries[1] { XCTAssertEqual(color, 1) } else { XCTFail() }
        if case .booking(_, _, _, _, let color, _) = entries[4] { XCTAssertEqual(color, 2) } else { XCTFail() }
        let summary = OwnerToday.daySummary(entries, pages: pages)
        XCTAssertEqual(summary.text, "2 встречи")
        XCTAssertEqual(summary.amount, 350_000)
        XCTAssertEqual(summary.currency, "RUB")
        // Завтра — без линии «сейчас».
        let tomorrow = OwnerToday.buildDay(bookings: bookings, tasks: [], free: [], pages: pages,
                                           day: date("2026-10-23T12:00:00Z"), now: now, calendar: utc)
        XCTAssertEqual(tomorrow.map(\.id), ["b4-2026-10-23T10:00:00Z"])
        XCTAssertEqual(OwnerToday.daySummary([], pages: pages).text, "Встреч нет")
    }

    // Цена записи (price/currency) — отдельно от суммы оплаты онлайн (payment.amount).

    private func priced(_ id: Int64, _ extra: String, status: String = "active") throws -> Booking {
        Booking(json: try decode("""
        {"id": \(id), "page_id": 2, "status": "\(status)"\(extra),
         "occurrences": [{"start": "2026-10-22T10:00:00Z", "end": "2026-10-22T11:00:00Z", "status": "active"}]}
        """))
    }

    func testBookingPriceApartFromPayment() throws {
        let b = try priced(1, #", "price": 300000, "currency": "RUB", "payment": {"status": "pending", "amount": 100000, "currency": "RUB", "kind": "deposit"}"#)
        let pg = try page(2, price: 250_000)
        XCTAssertEqual(b.price, 300_000)
        XCTAssertEqual(b.currency, "RUB")
        XCTAssertEqual(b.payment?.amount, 100_000)
        XCTAssertEqual(b.price(with: pg), 300_000)
        XCTAssertEqual(OwnerToday.priceText(b, page: pg), Money.format(300_000, currency: "RUB"))
        XCTAssertEqual(OwnerToday.paymentText(b.payment!), "Депозит \(Money.format(100_000, currency: "RUB"))")
        XCTAssertEqual(OwnerToday.paymentLine(b.payment!), "Депозит \(Money.format(100_000, currency: "RUB")) · ждём оплату")
        let full = BookingPayment(status: "paid", amount: 300_000, kind: "full")
        XCTAssertEqual(OwnerToday.paymentLine(full), "Предоплата \(Money.format(300_000, currency: "RUB")) · оплачено")
        XCTAssertEqual(OwnerToday.paymentText(BookingPayment(status: "paid", amount: 500, kind: "")),
                       "Оплата \(Money.format(500, currency: "RUB"))")
    }

    func testOldServerFallsBackToPagePrice() throws {
        let b = try priced(1, "")
        XCTAssertNil(b.price)
        XCTAssertEqual(b.currency, "")
        XCTAssertEqual(b.price(with: try page(2, price: 250_000)), 250_000)
        XCTAssertEqual(b.currency(with: try page(2, price: 250_000)), "RUB")
        XCTAssertEqual(b.price(with: nil), 0)
        XCTAssertEqual(b.currency(with: nil), "RUB")
        XCTAssertEqual(OwnerToday.priceText(b, page: nil), "")
        XCTAssertNil(try priced(2, #", "price": null"#).price, "null — как отсутствие поля")
    }

    func testZeroPriceAndPackageDoNotFallBack() throws {
        let pg = try page(2, price: 250_000)
        let free = try priced(1, #", "price": 0, "currency": "RUB""#)
        XCTAssertEqual(free.price(with: pg), 0)
        XCTAssertEqual(OwnerToday.priceText(free, page: pg), "")
        let pack = try priced(2, #", "price": 0, "package_id": 7"#)
        XCTAssertTrue(pack.byPackage)
        XCTAssertEqual(OwnerToday.priceText(pack, page: pg), "По абонементу")
        let oldPack = try priced(3, #", "package_id": 7"#)
        XCTAssertEqual(oldPack.price(with: pg), 0, "абонемент без поля price — не цена страницы")
    }

    func testDayTotalSkipsPackageSessions() throws {
        let pages = [try page(2, price: 250_000)]
        let bookings = [
            try priced(1, #", "price": 300000, "currency": "RUB", "payment": {"status": "pending", "amount": 100000, "kind": "deposit"}"#),
            try priced(2, #", "price": 300000, "currency": "RUB""#),
            try priced(3, #", "price": 0, "currency": "RUB", "package_id": 4"#),
            try priced(4, #", "price": 0, "currency": "RUB""#),
            try priced(5, ""), // старый сервер — цена страницы
            try priced(6, #", "price": 900000, "currency": "USD""#), // другая валюта не складывается
            try priced(7, #", "price": 300000, "currency": "RUB""#, status: "cancelled"),
        ]
        let total = OwnerToday.dayTotal(bookings, pages: pages)
        XCTAssertEqual(total.amount, 850_000)
        XCTAssertEqual(total.currency, "RUB")
        let now = date("2026-10-22T08:00:00Z")
        let entries = OwnerToday.buildDay(bookings: bookings, tasks: [], free: [], pages: pages,
                                          day: now, now: now, calendar: utc)
        let summary = OwnerToday.daySummary(entries, pages: pages)
        XCTAssertEqual(summary.text, "6 встреч")
        XCTAssertEqual(summary.amount, 850_000)
        XCTAssertEqual(OwnerToday.dayTotal([bookings[2], bookings[3]], pages: pages).amount, 0)
    }

    func testNextUpAndUntil() throws {
        let bookings = [
            try booking(1, page: 2, start: "2026-10-22T10:00:00Z", end: "2026-10-22T11:00:00Z"),
            try booking(2, page: 2, start: "2026-10-22T15:00:00Z", end: "2026-10-22T16:00:00Z"),
        ]
        let next = OwnerToday.nextUp(bookings, now: date("2026-10-22T10:30:00Z"))
        XCTAssertEqual(next?.booking.id, 1, "идущая сейчас встреча — следующая")
        let later = OwnerToday.nextUp(bookings, now: date("2026-10-22T11:00:00Z"))
        XCTAssertEqual(later?.booking.id, 2)
        XCTAssertNil(OwnerToday.nextUp(bookings, now: date("2026-10-23T00:00:00Z")))
        let now = date("2026-10-22T13:47:00Z")
        XCTAssertEqual(OwnerToday.untilText(date("2026-10-22T15:00:00Z"), now: now, calendar: utc), "через 1 ч 13 мин")
        XCTAssertEqual(OwnerToday.untilText(date("2026-10-22T14:00:00Z"), now: now, calendar: utc), "через 13 мин")
        XCTAssertEqual(OwnerToday.untilText(date("2026-10-22T13:00:00Z"), end: date("2026-10-22T14:00:00Z"),
                                            now: now, calendar: utc), "идёт сейчас")
        XCTAssertEqual(OwnerToday.untilText(date("2026-10-23T10:00:00Z"), now: now, calendar: utc), "завтра в 10:00")
        XCTAssertEqual(OwnerToday.untilText(date("2026-10-30T10:00:00Z"), now: now, calendar: utc), "пт, 30 октября")
    }
}

final class PackageTests: XCTestCase {

    func testOwnerPackage() throws {
        let p = OwnerPackage(json: try decode("""
        {"id": 11, "page_id": 2, "page_title": "Урок", "offer_id": 0, "title": "8 уроков", "sessions_total": 8,
         "sessions_used": 7, "sessions_left": 1, "valid_days": 60, "valid_until": "2026-12-01T00:00:00Z",
         "price": 1400000, "currency": "RUB", "status": "active", "payment": {"status": "paid", "amount": 1400000},
         "guest_name": "Сергей", "guest_email": "s@x.ru", "cc_email": "", "created_at": "2026-10-01T00:00:00Z"}
        """))
        XCTAssertEqual(p.remainingText, "осталось 1 из 8")
        XCTAssertEqual(p.statusView.label, "Заканчивается")
        XCTAssertEqual(p.payment?.status, "paid")
        XCTAssertEqual(p.usedFraction, 7.0 / 8.0, accuracy: 0.001)
        XCTAssertTrue(p.isOpen)
        var cancelled = p
        cancelled.status = "cancelled"
        XCTAssertEqual(cancelled.statusView.icon, .cross)
        XCTAssertFalse(cancelled.isOpen)
    }

    func testIssuePayloads() throws {
        let offer = PackageIssueRequest(pageId: 2, offerId: 1, guestName: " Сергей ", guestEmail: "s@x.ru", paid: true)
        XCTAssertNil(offer.validate())
        XCTAssertEqual(offer.payload, ["page_id": .number(2), "offer_id": .number(1), "guest_name": .string("Сергей"),
                                       "guest_email": .string("s@x.ru"), "paid": .bool(true)])
        let custom = PackageIssueRequest(pageId: 2, title: "Свой", sessions: 5, price: 500_000, validDays: 30,
                                         guestName: "Ольга", ccEmail: "mama@x.ru")
        XCTAssertNil(custom.validate())
        XCTAssertEqual(Set(custom.payload.keys), ["page_id", "title", "sessions", "price", "valid_days", "guest_name",
                                                  "cc_email", "paid"])
        XCTAssertNotNil(PackageIssueRequest(pageId: 2, title: "", guestName: "О").validate())
        XCTAssertNotNil(PackageIssueRequest(pageId: 2, offerId: 1, guestName: "").validate())
        XCTAssertNotNil(PackageIssueRequest(pageId: 2, offerId: 1, guestName: "О", guestEmail: "a@b.c", ccEmail: "A@b.c").validate())
        let result = PackageIssueResult(json: try decode("{\"package\": {\"id\": 3}, \"token\": \"tok\", \"url\": \"\"}"))
        XCTAssertEqual(result.package.id, 3)
        XCTAssertEqual(result.link, "#/p/tok")
        XCTAssertEqual(SelfvoyClient.packageListPayload(pageId: 0, status: "active", limit: 500),
                       ["limit": .number(200), "status": .string("active")])
    }
}

final class IntegrationsTests: XCTestCase {

    func testCalendarFeedUrl() {
        let feed = CalendarFeed(enabled: true, url: "", path: "/ics/abc.ics")
        XCTAssertEqual(feed.resolvedURL(serverURL: "wss://api.example.com"), "https://api.example.com/ics/abc.ics")
        XCTAssertEqual(feed.resolvedURL(serverURL: "ws://10.0.0.5:9000"), "http://10.0.0.5:9000/ics/abc.ics")
        XCTAssertNil(feed.resolvedURL(serverURL: "https://api.example.com"))
        XCTAssertNil(CalendarFeed(enabled: true).resolvedURL(serverURL: "wss://a.b"))
        let ready = CalendarFeed(enabled: true, url: "https://api.example.com/ics/x.ics", path: "/ics/x.ics")
        XCTAssertEqual(ready.resolvedURL(serverURL: "ws://localhost:9000"), "https://api.example.com/ics/x.ics")
        XCTAssertEqual(CalendarFeed.webcal("https://api.example.com/ics/x.ics"), "webcal://api.example.com/ics/x.ics")
        XCTAssertNil(CalendarFeed.webcal("http://a/ics/x.ics"))
    }

    func testTelegram() throws {
        let status = TelegramStatus(json: try decode("""
        {"enabled": true, "bot_username": "selfvoy_bot", "linked": true, "username": "anna"}
        """))
        XCTAssertEqual(status.summary, "Подключено · @anna")
        XCTAssertEqual(TelegramStatus().summary, "Бот не настроен на сервере")
        let link = TelegramLink(json: try decode("{\"url\": \"https://t.me/selfvoy_bot?start=o_1\", \"expires_at\": \"x\"}"))
        XCTAssertTrue(link.url.hasPrefix("https://t.me/"))
        // Объект notifications сервер заменяет целиком — прежние ключи сохраняются.
        let prefs = try decode("""
        {"notifications": {"quiet_hours": {"start": "22:00", "end": "08:00"}, "muted_kinds": ["login.new"]}}
        """)
        let payload = SelfvoyClient.telegramNotificationsPayload(current: prefs, enabled: false)
        XCTAssertEqual(payload["notifications"]?["telegram"], .bool(false))
        XCTAssertEqual(payload["notifications"]?["muted_kinds"], .array([.string("login.new")]))
        XCTAssertNotNil(payload["notifications"]?["quiet_hours"])
        XCTAssertNil(payload["preferences"])
        XCTAssertEqual(ServerInfo(json: try decode("{\"telegram_bot\": \"selfvoy_bot\"}")).telegramBot, "selfvoy_bot")
    }

    func testYooKassa() throws {
        XCTAssertNil(YooKassaCredentials.validate(shopId: "123456", secretKey: "test_abcdef"))
        XCTAssertNotNil(YooKassaCredentials.validate(shopId: "12a", secretKey: "test_abcdef"))
        XCTAssertNotNil(YooKassaCredentials.validate(shopId: "123", secretKey: "secret"))
        XCTAssertNotNil(YooKassaCredentials.validate(shopId: "123", secretKey: "live_ab cd"))
        XCTAssertEqual(YooKassaCredentials.payload(shopId: " 123 ", secretKey: "live_x1y2z3"),
                       ["provider": .string("yookassa"), "shop_id": .string("123"), "secret_key": .string("live_x1y2z3")])
        let status = PaymentsStatus(json: try decode("""
        {"connected": true, "provider": "yookassa", "shop_id": 123456, "connected_at": "2026-10-01T00:00:00Z"}
        """))
        XCTAssertEqual(status.shopId, "123456")
        XCTAssertTrue(status.connected)
    }

    func testSlotsPost() throws {
        XCTAssertEqual(SelfvoyClient.slotsPostPayload(pageId: 3, days: 30, format: .vk),
                       ["page_id": .number(3), "days": .number(14), "format": .string("vk")])
        let post = SlotsPost(json: try decode("{\"text\": \"Свободные окна\", \"slots\": 4, \"url\": \"\"}"))
        XCTAssertEqual(post.slots, 4)
        XCTAssertEqual(post.shareText(fallbackLink: "https://s.example/#/b/x"),
                       "Свободные окна\n\nЗаписаться: https://s.example/#/b/x")
        XCTAssertEqual(SlotsPost(text: "Т https://a", slots: 1, url: "https://a").shareText(fallbackLink: "https://b"), "Т https://a")
    }

    func testNotificationKinds() throws {
        XCTAssertEqual(SelfvoyNotificationKind.status("booking.confirmed").label, "Придёт")
        XCTAssertEqual(SelfvoyNotificationKind.status("booking.payment", payload: try decode("{\"status\": \"refund_due\"}")).label,
                       "Верните оплату")
        XCTAssertEqual(SelfvoyNotificationKind.status("package.low").tone, .amber)
        XCTAssertTrue(SelfvoyNotificationKind.touchesBookings("booking.payment"))
        XCTAssertTrue(SelfvoyNotificationKind.touchesPackages("package.purchased"))
        XCTAssertTrue(SelfvoyNotificationKind.touchesPlan("referral.reward"))
    }
}
