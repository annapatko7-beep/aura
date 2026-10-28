// OwnerToday.swift — «Сегодня» владельца: следующая встреча, лента дня (встречи
// записей, задачи со сроком, свободные окна, линия «сейчас») и история записи
// для карточки. Чистая логика без UI — тестируется в SelfvoyKitTests.
// Зеркало web/src/lib/today.ts и bookingHistory из web/src/lib/payment.ts.
import Foundation

/// Свободное окно страницы записи (то же, что видит гость в `booking.get`).
public struct FreeSlot: Equatable, Hashable, Sendable {
    public var start: String
    public var end: String
    public var pageId: Int64

    public init(start: String, end: String, pageId: Int64) {
        self.start = start
        self.end = end
        self.pageId = pageId
    }
}

/// Задача со сроком для ленты дня (UI-слой переводит в неё свои задачи).
public struct DayTaskInput: Equatable, Sendable {
    public var id: Int64
    public var title: String
    public var at: Date

    public init(id: Int64, title: String, at: Date) {
        self.id = id
        self.title = title
        self.at = at
    }
}

public enum DayEntry: Identifiable, Equatable, Sendable {
    case booking(booking: Booking, occurrence: BookingOccurrence, start: Date, end: Date, color: Int, status: StatusView)
    case task(DayTaskInput)
    case free(start: Date, end: Date, pageId: Int64)
    case now(Date)

    public var id: String {
        switch self {
        case .booking(let booking, let occurrence, _, _, _, _): return "b\(booking.id)-\(occurrence.start)"
        case .task(let task): return "t\(task.id)"
        case .free(let start, _, let pageId): return "f\(pageId)-\(Int(start.timeIntervalSince1970))"
        case .now: return "now"
        }
    }

    public var start: Date {
        switch self {
        case .booking(_, _, let start, _, _, _): return start
        case .task(let task): return task.at
        case .free(let start, _, _): return start
        case .now(let date): return date
        }
    }

    var isFree: Bool {
        if case .free = self { return true }
        return false
    }
}

public struct NextAppointment: Equatable, Sendable {
    public var booking: Booking
    public var occurrence: BookingOccurrence
    public var start: Date
    public var end: Date
}

public struct HistoryStep: Identifiable, Equatable, Sendable {
    public enum Tone: String, Sendable { case done, wait, fail, info }
    public var id: String
    public var title: String
    public var detail: String
    public var tone: Tone
}

public enum OwnerToday {
    /// Цветов капсул (тип встречи = страница записи), по кругу.
    public static let capsuleColors = 5
    /// Сколько свободных окон показывать в ленте дня.
    public static let freeInDay = 3

    /// Номер цвета страницы 1...5 — по порядку создания (id по возрастанию).
    public static func pageColor(_ pageId: Int64, pages: [BookingPage]) -> Int {
        let ids = Array(Set(pages.map(\.id))).sorted()
        let index = ids.firstIndex(of: pageId).map { Int($0) } ?? Int(abs(pageId) % Int64(capsuleColors))
        return index % capsuleColors + 1
    }

    /// Статус встречи — значок и слово. Важнее всего то, что требует действия: оплата,
    /// затем подтверждение гостя; новая запись (меньше суток) — «внимание».
    public static func occurrenceStatus(_ booking: Booking, _ occurrence: BookingOccurrence,
                                        now: Date = Date()) -> StatusView {
        if booking.status == "cancelled" || occurrence.status == "cancelled" {
            return StatusView("Отменена", .red, .cross)
        }
        if let payment = booking.payment, payment.awaiting, let view = payment.ownerStatus { return view }
        if occurrence.confirmed { return StatusView("Подтвердил(а)", .green, .check) }
        if let end = SelfvoyDate.parse(occurrence.end) ?? SelfvoyDate.parse(occurrence.start), end <= now {
            return StatusView("Прошла", .neutral, .check)
        }
        if let created = SelfvoyDate.parse(booking.createdAt), now.timeIntervalSince(created) < 24 * 3600 {
            return StatusView("Новая запись", .attention, .plus)
        }
        if booking.payment?.status == "paid" { return StatusView("Оплачено", .green, .check) }
        return StatusView("Записан(а)", .neutral, .calendar)
    }

    /// Ближайшая встреча: идущая сейчас или следующая.
    public static func nextUp(_ bookings: [Booking], now: Date = Date()) -> NextAppointment? {
        var best: NextAppointment?
        for booking in bookings where booking.isActive {
            for occurrence in booking.occurrences where occurrence.active {
                guard let start = SelfvoyDate.parse(occurrence.start) else { continue }
                let end = SelfvoyDate.parse(occurrence.end) ?? start
                if end <= now { continue }
                if best == nil || start < best!.start {
                    best = NextAppointment(booking: booking, occurrence: occurrence, start: start, end: end)
                }
            }
        }
        return best
    }

    /// «через 1 ч 13 мин», «идёт сейчас», «завтра в 10:00», «пт, 23 октября».
    public static func untilText(_ start: Date, end: Date? = nil, now: Date = Date(),
                                 calendar: Calendar = .current) -> String {
        let minutes = Int((start.timeIntervalSince(now) / 60).rounded())
        if minutes <= 0 {
            if let end, end > now { return "идёт сейчас" }
            return "началась"
        }
        if calendar.isDate(start, inSameDayAs: now) {
            if minutes < 60 { return "через \(minutes) мин" }
            let rest = minutes % 60
            return "через \(minutes / 60) ч" + (rest > 0 ? " \(rest) мин" : "")
        }
        let zone = calendar.timeZone
        if let tomorrow = calendar.date(byAdding: .day, value: 1, to: now), calendar.isDate(start, inSameDayAs: tomorrow) {
            return "завтра в \(SelfvoyDate.time(start, zone: zone))"
        }
        return "\(SelfvoyDate.weekdayShort(start, zone: zone)), \(SelfvoyDate.dayMonth(start, zone: zone))"
    }

    /// Лента дня `day`: встречи, задачи (кроме созданных самой записью), свободные окна
    /// (будущие, не пересекаются со встречами, не больше freeInDay) и «сейчас», если день сегодняшний.
    public static func buildDay(bookings: [Booking], tasks: [DayTaskInput], free: [FreeSlot], pages: [BookingPage],
                                day: Date, now: Date = Date(), calendar: Calendar = .current) -> [DayEntry] {
        var entries: [DayEntry] = []
        var busy: [(Date, Date)] = []
        var bookingMinutes = Set<Int>()
        for booking in bookings {
            for occurrence in booking.occurrences {
                if let start = SelfvoyDate.parse(occurrence.start) {
                    bookingMinutes.insert(Int(start.timeIntervalSince1970 / 60))
                }
                guard booking.isActive, occurrence.active,
                      let start = SelfvoyDate.parse(occurrence.start),
                      calendar.isDate(start, inSameDayAs: day) else { continue }
                let end = SelfvoyDate.parse(occurrence.end) ?? start
                busy.append((start, end))
                entries.append(.booking(booking: booking, occurrence: occurrence, start: start, end: end,
                                        color: pageColor(booking.pageId, pages: pages),
                                        status: occurrenceStatus(booking, occurrence, now: now)))
            }
        }
        for task in tasks where calendar.isDate(task.at, inSameDayAs: day) {
            if bookingMinutes.contains(Int(task.at.timeIntervalSince1970 / 60)) { continue }
            entries.append(.task(task))
        }
        var shown = 0
        var seen = Set<Date>()
        let sorted = free.compactMap { slot -> (Date, Date, Int64)? in
            guard let start = SelfvoyDate.parse(slot.start), let end = SelfvoyDate.parse(slot.end) else { return nil }
            return (start, end, slot.pageId)
        }.sorted { $0.0 < $1.0 }
        for (start, end, pageId) in sorted {
            if shown >= freeInDay { break }
            guard calendar.isDate(start, inSameDayAs: day), start > now, !seen.contains(start) else { continue }
            if busy.contains(where: { start < $0.1 && end > $0.0 }) { continue }
            seen.insert(start)
            busy.append((start, end))
            entries.append(.free(start: start, end: end, pageId: pageId))
            shown += 1
        }
        entries.sort { a, b in
            if a.start != b.start { return a.start < b.start }
            return !a.isFree && b.isFree
        }
        if calendar.isDate(day, inSameDayAs: now), !entries.isEmpty {
            if let index = entries.firstIndex(where: { $0.start > now }) {
                entries.insert(.now(now), at: index)
            } else {
                entries.append(.now(now))
            }
        }
        return entries
    }

    /// «3 встречи» и их стоимость: цены записей (старый сервер — цены страниц), без занятий
    /// по абонементу (оплачены пакетом); одна валюта — первая встреченная.
    public static func daySummary(_ entries: [DayEntry], pages: [BookingPage]) -> (text: String, amount: Int, currency: String) {
        var count = 0
        var bookings: [Booking] = []
        for entry in entries {
            guard case .booking(let booking, _, _, _, _, _) = entry else { continue }
            count += 1
            bookings.append(booking)
        }
        let total = dayTotal(bookings, pages: pages)
        let text = count > 0 ? "\(count) \(SelfvoyDate.plural(count, "встреча", "встречи", "встреч"))" : "Встреч нет"
        return (text, total.amount, total.currency)
    }

    /// Сумма встреч: активные записи без абонемента, цена > 0, одна валюта.
    public static func dayTotal(_ bookings: [Booking], pages: [BookingPage]) -> (amount: Int, currency: String) {
        var amount = 0
        var currency = ""
        for booking in bookings where booking.isActive && !booking.byPackage {
            let page = pages.first { $0.id == booking.pageId }
            let price = booking.price(with: page)
            guard price > 0 else { continue }
            let code = booking.currency(with: page)
            if !currency.isEmpty && code != currency { continue }
            currency = code
            amount += price
        }
        return (amount, currency)
    }

    public static let byPackageText = "По абонементу"

    /// Главное число карточки: «3 000 ₽» · «По абонементу» · "" (бесплатно / цены нет).
    public static func priceText(_ booking: Booking, page: BookingPage?) -> String {
        if booking.byPackage { return byPackageText }
        let amount = booking.price(with: page)
        return amount > 0 ? Money.format(amount, currency: booking.currency(with: page)) : ""
    }

    /// «Депозит 1 000 ₽» · «Предоплата 3 000 ₽» · «Оплата 3 000 ₽».
    public static func paymentText(_ payment: BookingPayment) -> String {
        let what: String
        switch payment.kind {
        case "deposit": what = "Депозит"
        case "full": what = "Предоплата"
        default: what = "Оплата"
        }
        return "\(what) \(payment.amountText)"
    }

    /// Строка оплаты под ценой: «Депозит 1 000 ₽ · ждём оплату» (в UI — ещё и значок статуса).
    public static func paymentLine(_ payment: BookingPayment) -> String {
        guard let status = payment.ownerStatus else { return paymentText(payment) }
        return "\(paymentText(payment)) · \(status.label.prefix(1).lowercased() + status.label.dropFirst())"
    }

    /// Встреча, о которой рассказывает карточка: выбранная или ближайшая активная.
    public static func focusOccurrence(_ booking: Booking, start: String?, now: Date = Date()) -> BookingOccurrence? {
        let list = booking.occurrences
        if let start, let found = list.first(where: { $0.start == start }) { return found }
        return list.first { occurrence in
            occurrence.active && (SelfvoyDate.parse(occurrence.end.isEmpty ? occurrence.start : occurrence.end)
                .map { $0 > now } ?? false)
        } ?? list.last
    }

    /// Шаги истории записи: запись → оплата → напоминания → «Приду» → отмена.
    public static func history(_ booking: Booking, start: String? = nil, now: Date = Date(),
                               calendar: Calendar = .current) -> [HistoryStep] {
        let when = { (iso: String) in SelfvoyDate.when(iso, now: now, calendar: calendar) }
        var steps = [HistoryStep(id: "created", title: "Записался(ась) по ссылке", detail: when(booking.createdAt), tone: .done)]
        if let payment = booking.payment {
            let amount = payment.amountText
            let kind = payment.kind == "deposit" ? "депозит" : "оплату"
            switch payment.status {
            case "pending":
                let via = payment.method == "yookassa" ? " · ЮKassa" : " · по ссылке"
                steps.append(HistoryStep(id: "payment", title: "Ждём \(kind) \(amount)",
                                         detail: payment.dueAt.isEmpty ? "" : "до \(when(payment.dueAt))\(via)", tone: .wait))
            case "claimed":
                steps.append(HistoryStep(id: "payment", title: "Гость отметил: оплатил(а) \(amount)",
                                         detail: "Проверьте поступление и отметьте оплату", tone: .wait))
            case "paid":
                steps.append(HistoryStep(id: "payment", title: "Оплачено \(amount)",
                                         detail: payment.paidAt.isEmpty ? "" : when(payment.paidAt), tone: .done))
            case "waived":
                steps.append(HistoryStep(id: "payment", title: "Без оплаты",
                                         detail: "Вы отметили, что оплата не нужна", tone: .info))
            case "refunded":
                steps.append(HistoryStep(id: "payment", title: "Возврат \(amount)",
                                         detail: payment.refundedAt.isEmpty ? "" : when(payment.refundedAt), tone: .info))
            case "expired":
                steps.append(HistoryStep(id: "payment", title: "Оплата не поступила к сроку",
                                         detail: payment.dueAt.isEmpty ? "" : "срок был \(when(payment.dueAt))", tone: .fail))
            default:
                break
            }
        }
        if let focus = focusOccurrence(booking, start: start, now: now) {
            let live = booking.isActive && focus.active
            let reminders = booking.reminders.filter { $0.start == focus.start }.sorted { $0.dueAt < $1.dueAt }
            for reminder in reminders {
                let what = reminder.kind == "24h" ? "за сутки" : "за 2 часа"
                if !reminder.sentAt.isEmpty {
                    steps.append(HistoryStep(id: "rem-\(reminder.kind)", title: "Напоминание \(what) отправлено",
                                             detail: when(reminder.sentAt), tone: .done))
                } else if live {
                    steps.append(HistoryStep(id: "rem-\(reminder.kind)", title: "Напоминание \(what)",
                                             detail: "запланировано на \(when(reminder.dueAt))", tone: .info))
                }
            }
            if focus.confirmed {
                steps.append(HistoryStep(id: "confirmed", title: "Подтвердил(а): «Приду»", detail: when(focus.confirmedAt), tone: .done))
            } else if live, let begin = SelfvoyDate.parse(focus.start), begin > now {
                steps.append(HistoryStep(id: "confirmed", title: "Ещё не подтвердил(а)",
                                         detail: "Кнопка «Приду» — в напоминании", tone: .info))
            }
            if focus.status == "cancelled" && booking.isActive {
                steps.append(HistoryStep(id: "cancelled", title: "Эта встреча отменена", detail: "", tone: .fail))
            }
        }
        if booking.status == "cancelled" {
            let title: String
            switch booking.cancelledBy {
            case "owner": title = "Вы отменили запись"
            case "system": title = "Запись отменена: оплата не поступила"
            default: title = "Гость отменил запись"
            }
            steps.append(HistoryStep(id: "cancelled", title: title, detail: when(booking.updatedAt), tone: .fail))
        }
        return steps
    }
}
