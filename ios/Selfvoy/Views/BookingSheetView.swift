// BookingSheetView.swift — карточка записи владельца (окно снизу, как
// web/src/components/BookingSheet.tsx): кто, когда, сколько стоит, статус оплаты,
// история (запись → оплата → напоминания → «Приду»), действия: написать,
// отменить встречу или всю серию, отметить оплату / «без оплаты» / возврат
// (booking.ownerCancel, booking.paymentSet).
import SelfvoyKit
import SwiftUI

struct BookingSheetView: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.openURL) private var openURL
    @Environment(\.dismiss) private var dismiss
    let focus: BookingFocus

    @State private var busy = ""
    @State private var error = ""
    @State private var confirmCancel: CancelScope?

    private enum CancelScope: Identifiable {
        case one, all
        var id: Int { self == .one ? 1 : 2 }
    }

    private var booking: Booking? { store.bookings.first { $0.id == focus.bookingId } }

    var body: some View {
        Group {
            if let booking {
                content(booking)
            } else {
                Text("Запись не найдена").font(InkFont.callout).foregroundStyle(Ink.secondary).padding(32)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(Ink.bg.ignoresSafeArea())
    }

    private func content(_ booking: Booking) -> some View {
        let now = Date()
        let occurrence = OwnerToday.focusOccurrence(booking, start: focus.start, now: now)
        let upcoming = booking.occurrences.filter { item in
            item.active && (SelfvoyDate.parse(item.start).map { $0 > now } ?? false)
        }
        let focusUpcoming = occurrence.map { item in upcoming.contains(item) } ?? false
        let page = store.page(id: booking.pageId)
        let steps = OwnerToday.history(booking, start: occurrence?.start, now: now)
        return ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                header(booking, occurrence: occurrence)
                if let occurrence { whenCard(booking, occurrence: occurrence, page: page, now: now) }
                actions(booking, focusUpcoming: focusUpcoming, upcomingCount: upcoming.count)
                if !error.isEmpty { InkNotice(text: error) }
                historyCard(steps)
                if booking.occurrences.count > 1 { seriesCard(booking) }
                guestCard(booking)
                paymentButtons(booking)
            }
            .padding(20)
        }
        .confirmationDialog(confirmTitle(booking, upcoming: upcoming.count, occurrence: occurrence),
                            isPresented: Binding(get: { confirmCancel != nil }, set: { if !$0 { confirmCancel = nil } }),
                            titleVisibility: .visible) {
            Button(confirmCancel == .all ? "Отменить все встречи" : "Отменить встречу", role: .destructive) {
                let scope = confirmCancel
                confirmCancel = nil
                run("cancel") {
                    try await store.cancelBooking(booking, occurrence: scope == .one ? occurrence?.start : nil)
                }
            }
            Button("Не отменять", role: .cancel) { confirmCancel = nil }
        }
    }

    // MARK: Шапка

    private func header(_ booking: Booking, occurrence: BookingOccurrence?) -> some View {
        HStack(alignment: .top, spacing: 12) {
            InkAvatar(name: booking.guestName, size: 52)
            VStack(alignment: .leading, spacing: 2) {
                Text(booking.guestName.isEmpty ? "Гость" : booking.guestName)
                    .font(InkFont.title).foregroundStyle(Ink.label)
                Text(subtitle(booking, occurrence: occurrence))
                    .font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
            Spacer(minLength: 0)
            Button {
                dismiss()
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(Ink.secondary)
                    .frame(width: 30, height: 30)
                    .background(Circle().fill(Ink.fill))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Закрыть")
        }
    }

    private func subtitle(_ booking: Booking, occurrence: BookingOccurrence?) -> String {
        var parts = [booking.pageTitle.isEmpty ? "Запись" : booking.pageTitle]
        if let occurrence, let start = SelfvoyDate.parse(occurrence.start), let end = SelfvoyDate.parse(occurrence.end) {
            parts.append("\(Int(end.timeIntervalSince(start) / 60)) мин")
        }
        if booking.packageId > 0 { parts.append("по абонементу") }
        return parts.joined(separator: " · ")
    }

    // MARK: Когда и статус

    private func whenCard(_ booking: Booking, occurrence: BookingOccurrence, page: BookingPage?, now: Date) -> some View {
        let index = (booking.occurrences.firstIndex(of: occurrence) ?? 0) + 1
        let live = booking.isActive && occurrence.active
        let status = live ? OwnerToday.occurrenceStatus(booking, occurrence, now: now)
                          : StatusView(booking.isActive ? "Встреча отменена" : cancelledLabel(booking), .red, .cross)
        let payStatus = booking.payment?.ownerStatus
        return InkGroup {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(dayTitle(occurrence.start)).font(InkFont.headline).foregroundStyle(Ink.label)
                    Text(range(occurrence) + (booking.occurrences.count > 1 ? " · встреча \(index) из \(booking.occurrences.count)" : ""))
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                Spacer(minLength: 8)
                // Главное число — цена услуги из записи; «По абонементу» — без денег.
                let price = OwnerToday.priceText(booking, page: page)
                if !price.isEmpty {
                    Text(price).font(InkFont.headline.monospacedDigit()).foregroundStyle(Ink.label)
                }
            }
            .padding(14)
            if let payment = booking.payment {
                // Оплата онлайн — отдельной строкой: «Депозит 1 000 ₽» и статус (значок + слово).
                InkDivider(inset: 14)
                HStack {
                    Text(OwnerToday.paymentText(payment)).font(InkFont.callout).foregroundStyle(Ink.secondary)
                    Spacer(minLength: 8)
                    if let payStatus { StatusBadge(status: payStatus) }
                }
                .padding(14)
            }
            InkDivider(inset: 14)
            HStack {
                Text("Статус").font(InkFont.callout).foregroundStyle(Ink.secondary)
                Spacer(minLength: 8)
                StatusBadge(status: status)
            }
            .padding(14)
        }
    }

    private func cancelledLabel(_ booking: Booking) -> String {
        switch booking.cancelledBy {
        case "owner": return "Отменили вы"
        case "system": return "Не оплачено — отменена"
        default: return "Отменил гость"
        }
    }

    private func dayTitle(_ iso: String) -> String {
        guard let date = SelfvoyDate.parse(iso) else { return "" }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "ru_RU")
        formatter.dateFormat = "EEEEEE, d MMMM"
        let text = formatter.string(from: date)
        return text.prefix(1).uppercased() + text.dropFirst()
    }

    private func range(_ occurrence: BookingOccurrence) -> String {
        guard let start = SelfvoyDate.parse(occurrence.start) else { return "" }
        guard let end = SelfvoyDate.parse(occurrence.end) else { return SelfvoyDate.time(start) }
        return "\(SelfvoyDate.time(start))–\(SelfvoyDate.time(end))"
    }

    // MARK: Действия

    private func actions(_ booking: Booking, focusUpcoming: Bool, upcomingCount: Int) -> some View {
        HStack(spacing: 10) {
            actionTile("Написать", symbol: "envelope.fill", tint: Ink.accent, enabled: !booking.guestEmail.isEmpty) {
                let email = booking.guestEmail.addingPercentEncoding(withAllowedCharacters: .urlUserAllowed) ?? booking.guestEmail
                if let url = URL(string: "mailto:\(email)") { openURL(url) }
            }
            if focusUpcoming && booking.isActive {
                actionTile(upcomingCount > 1 ? "Отменить эту" : "Отменить", symbol: "xmark", tint: Ink.red,
                           enabled: busy.isEmpty) {
                    confirmCancel = .one
                }
            }
            if upcomingCount > 1 && booking.isActive {
                actionTile("Отменить все", symbol: "nosign", tint: Ink.red, enabled: busy.isEmpty) {
                    confirmCancel = .all
                }
            }
        }
    }

    private func actionTile(_ title: String, symbol: String, tint: Color, enabled: Bool,
                            action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: symbol).font(.system(size: 18, weight: .semibold))
                Text(title).font(.system(size: 12, weight: .semibold)).lineLimit(1).minimumScaleFactor(0.8)
            }
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity, minHeight: 64)
            .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Ink.card))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.45)
    }

    private func confirmTitle(_ booking: Booking, upcoming: Int, occurrence: BookingOccurrence?) -> String {
        if confirmCancel == .all {
            var text = "Отменить все предстоящие встречи (\(upcoming))? Гость получит письмо."
            if booking.payment?.status == "paid" { text += " Оплата вернётся гостю." }
            return text
        }
        let when = occurrence.map { SelfvoyDate.when($0.start) } ?? ""
        return "Отменить встречу \(when)? Гость получит письмо."
    }

    // MARK: История

    private func historyCard(_ steps: [HistoryStep]) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            ForEach(steps) { step in
                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: symbol(step.tone))
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(color(step.tone))
                        .frame(width: 22)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(step.title).font(InkFont.headline).foregroundStyle(Ink.label)
                        if !step.detail.isEmpty {
                            Text(step.detail).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                        }
                    }
                }
                .accessibilityElement(children: .combine)
            }
        }
        .inkCard()
        .accessibilityLabel("История записи")
    }

    private func symbol(_ tone: HistoryStep.Tone) -> String {
        switch tone {
        case .done: return "checkmark.circle.fill"
        case .wait: return "clock.fill"
        case .fail: return "xmark.circle.fill"
        case .info: return "bell.circle.fill"
        }
    }

    private func color(_ tone: HistoryStep.Tone) -> Color {
        switch tone {
        case .done: return Ink.green
        case .wait: return Ink.amberInk
        case .fail: return Ink.red
        case .info: return Ink.secondary
        }
    }

    private func seriesCard(_ booking: Booking) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Встречи серии").font(InkFont.headline).foregroundStyle(Ink.label)
            ForEach(booking.occurrences, id: \.start) { item in
                HStack {
                    Text(SelfvoyDate.when(item.start)).font(InkFont.callout)
                        .strikethrough(!item.active)
                        .foregroundStyle(item.active ? Ink.label : Ink.tertiary)
                    Spacer()
                    if !item.active {
                        StatusBadge(status: StatusView("Отменена", .red, .cross), compact: true)
                    } else if item.confirmed {
                        StatusBadge(status: StatusView("Придёт", .green, .check), compact: true)
                    }
                }
            }
        }
        .inkCard()
    }

    @ViewBuilder
    private func guestCard(_ booking: Booking) -> some View {
        if !booking.guestEmail.isEmpty || !booking.ccEmail.isEmpty || !booking.guestComment.isEmpty {
            VStack(alignment: .leading, spacing: 10) {
                if !booking.guestEmail.isEmpty { labeled("Email", booking.guestEmail) }
                if !booking.ccEmail.isEmpty { labeled("Копия писем", booking.ccEmail) }
                if !booking.guestComment.isEmpty {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Комментарий").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                        Text(booking.guestComment).font(InkFont.callout).foregroundStyle(Ink.label)
                    }
                }
            }
            .inkCard()
        }
    }

    private func labeled(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).font(InkFont.callout).foregroundStyle(Ink.secondary)
            Spacer(minLength: 8)
            Text(value).font(InkFont.callout).foregroundStyle(Ink.label).lineLimit(1).truncationMode(.middle)
                .textSelection(.enabled)
        }
    }

    // MARK: Оплата

    @ViewBuilder
    private func paymentButtons(_ booking: Booking) -> some View {
        if let payment = booking.payment, !payment.ownerActions.isEmpty {
            VStack(spacing: 8) {
                ForEach(Array(payment.ownerActions.enumerated()), id: \.element) { index, mark in
                    Button {
                        run("pay-\(mark.rawValue)") { try await store.setBookingPayment(booking, mark) }
                    } label: {
                        HStack(spacing: 8) {
                            if busy == "pay-\(mark.rawValue)" {
                                ProgressView().tint(index == 0 && mark == .paid ? .white : Ink.accent)
                            } else if mark == .paid {
                                Image(systemName: "checkmark")
                            }
                            Text(mark.title)
                        }
                    }
                    .buttonStyle(PaymentButtonStyle(primary: index == 0 && mark == .paid, danger: mark == .refunded))
                    .disabled(!busy.isEmpty)
                }
                if payment.method == "yookassa" && payment.ownerActions.contains(.refunded) {
                    Text("Отметка «Возврат» не переводит деньги: вернуть оплату через ЮKassa можно, отменив запись целиком.")
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                if payment.status == "pending" && !payment.dueAt.isEmpty {
                    Text("Время держится до \(SelfvoyDate.when(payment.dueAt)); не оплачено — запись отменится сама.")
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
            }
        }
    }

    private func run(_ key: String, _ work: @escaping () async throws -> Void) {
        busy = key
        error = ""
        Task { @MainActor in
            do {
                try await work()
            } catch {
                self.error = store.describe(error)
            }
            busy = ""
        }
    }
}

private struct PaymentButtonStyle: ButtonStyle {
    let primary: Bool
    let danger: Bool

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .semibold))
            .foregroundStyle(primary ? Color.white : danger ? Ink.red : Ink.accent)
            .frame(maxWidth: .infinity, minHeight: 50)
            .background(Capsule().fill(primary ? Ink.accent : (danger ? Ink.red : Ink.accent).opacity(0.12)))
            .opacity(configuration.isPressed ? 0.8 : 1)
    }
}
