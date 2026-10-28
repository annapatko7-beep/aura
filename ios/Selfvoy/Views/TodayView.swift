// TodayView.swift — «Сегодня» владельца (дизайн 2026, как web/src/pages/Home.tsx):
// карточка следующей встречи на акценте и лента дня — встречи цветными капсулами
// на пунктирной линии, красная линия «сейчас», задачи со сроком и пустые окна
// пунктирными капсулами с «Предложить». Нажатие на встречу — карточка записи.
import SelfvoyKit
import SwiftUI

struct TodayView: View {

    @EnvironmentObject private var store: AppStore
    @State private var dayOffset = 0
    @State private var focus: BookingFocus?

    private static let eyebrowFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "ru_RU")
        formatter.dateFormat = "EEEE, d MMMM"
        return formatter
    }()

    var body: some View {
        // Минутная стрелка: «через N мин» и линия «сейчас» живые.
        TimelineView(.periodic(from: .now, by: 30)) { context in
            content(now: context.date)
        }
        .inkScreen()
        .sheet(item: $focus) { target in
            BookingSheetView(focus: target)
                .inkSheet()
        }
        .onAppear { store.loadBooking() }
    }

    private func content(now: Date) -> some View {
        let pages = store.bookingPages ?? []
        let calendar = Calendar.current
        let day = calendar.date(byAdding: .day, value: dayOffset, to: calendar.startOfDay(for: now)) ?? now
        let entries = OwnerToday.buildDay(bookings: store.bookings, tasks: store.dayTasks, free: store.freeSlots,
                                          pages: pages, day: day, now: now)
        let summary = OwnerToday.daySummary(entries, pages: pages)
        let next = OwnerToday.nextUp(store.bookings, now: now)
        return ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                header(now: now)
                if !store.confirmations.isEmpty { confirmationsBanner }
                if let next {
                    NextAppointmentCard(next: next, now: now,
                                        color: OwnerToday.pageColor(next.booking.pageId, pages: pages),
                                        price: price(for: next.booking, pages: pages)) {
                        focus = BookingFocus(bookingId: next.booking.id, start: next.occurrence.start)
                    }
                } else {
                    emptyNext(pages: pages)
                }
                InkSectionHeader(title: "День", trailing: summary.amount > 0
                                 ? "\(summary.text) · \(Money.format(summary.amount, currency: summary.currency))"
                                 : summary.text)
                Picker("День", selection: $dayOffset) {
                    Text("Сегодня").tag(0)
                    Text("Завтра").tag(1)
                }
                .pickerStyle(.segmented)
                if entries.isEmpty {
                    InkEmpty(text: dayOffset == 0 ? "На сегодня ни встреч, ни задач со сроком." : "На завтра пока ничего нет.")
                } else {
                    DayTimeline(entries: entries, now: now, onOpen: { booking, start in
                        focus = BookingFocus(bookingId: booking.id, start: start)
                    }, onToggleTask: { id in
                        store.setTaskStatus(id: id, status: "complete")
                    }, offerText: { start, end, pageId in
                        offerText(start: start, end: end, pageId: pageId, now: now)
                    })
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
        .refreshable { store.loadBooking(); store.loadTasks() }
    }

    // MARK: Шапка

    private func header(now: Date) -> some View {
        InkHeader(eyebrow: Self.eyebrowFormatter.string(from: now), title: "Сегодня") {
            InkRoundButton(systemImage: "bell", label: "Уведомления",
                           badge: Int(store.unreadNotifications)) {
                store.selectTab(.notifications)
            }
            .accessibilityIdentifier("home.notifications")
            Button {
                store.selectTab(.settings)
            } label: {
                InkAvatar(name: store.userName, size: 38)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Настройки")
            .accessibilityIdentifier("home.settings")
        }
    }

    private var confirmationsBanner: some View {
        Button {
            store.selectTab(.confirmations)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: "exclamationmark.shield.fill").foregroundStyle(Ink.amberInk)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Ждут вашего решения: \(store.confirmations.count)")
                        .font(InkFont.headline).foregroundStyle(Ink.label)
                    if let first = store.confirmations.first {
                        Text(first.summary).font(InkFont.footnote).foregroundStyle(Ink.secondary).lineLimit(1)
                    }
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right").foregroundStyle(Ink.tertiary)
            }
            .inkCard(padding: 14)
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("home.confirmations")
    }

    private func emptyNext(pages: [BookingPage]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Следующая встреча").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
            Text(store.bookingsLoaded ? "Предстоящих встреч нет" : "Загружаем записи…")
                .font(InkFont.title).foregroundStyle(Ink.label)
            Text(pages.isEmpty ? "Заведите страницу записи — клиенты будут записываться сами."
                               : "Поделитесь ссылкой записи или постом со свободными окнами.")
                .font(InkFont.callout).foregroundStyle(Ink.secondary)
            Button(pages.isEmpty ? "Создать страницу" : "Пост со свободными окнами") {
                store.bookingIntent = pages.isEmpty ? .newPage : .post
                store.selectTab(.booking)
            }
            .buttonStyle(InkSecondaryButtonStyle())
            .padding(.top, 6)
        }
        .inkCard(padding: 18, radius: 26)
    }

    /// Главное число карточки — цена услуги из записи (не сумма оплаты онлайн).
    private func price(for booking: Booking, pages: [BookingPage]) -> String {
        OwnerToday.priceText(booking, page: pages.first(where: { $0.id == booking.pageId }))
    }

    /// Текст «Предложить» для свободного окна: время и ссылка записи.
    private func offerText(start: Date, end: Date, pageId: Int64, now: Date) -> String {
        let page = store.page(id: pageId)
        let title = page?.fields.title ?? "встреча"
        let day = Calendar.current.isDate(start, inSameDayAs: now) ? "сегодня" : "завтра"
        var text = "Есть свободное окно \(day) в \(SelfvoyDate.time(start)) — \(title)."
        if let page, let link = store.publicLink(page) { text += " Записаться: \(link)" }
        return text
    }
}

// MARK: - Карточка «Следующая встреча»

private struct NextAppointmentCard: View {
    @Environment(\.openURL) private var openURL
    let next: NextAppointment
    let now: Date
    let color: Int
    let price: String
    let onOpen: () -> Void

    private var minutes: Int { Int((next.end.timeIntervalSince(next.start) / 60).rounded()) }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("Следующая · \(OwnerToday.untilText(next.start, end: next.end, now: now))")
                Spacer(minLength: 8)
                Text(next.booking.pageTitle).lineLimit(1)
            }
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.white.opacity(0.85))
            Text(SelfvoyDate.time(next.start))
                .font(InkFont.heroTime)
                .foregroundStyle(.white)
            (Text(next.booking.guestName.isEmpty ? "Гость" : next.booking.guestName).bold()
             + Text(" · \(minutes) мин" + (price.isEmpty ? "" : " · \(price)")))
                .font(InkFont.callout)
                .foregroundStyle(.white)
            if let payment = next.booking.payment {
                // Оплата онлайн — второстепенно: значок статуса + «Депозит 1 000 ₽ · ждём оплату».
                HStack(spacing: 4) {
                    if let status = payment.ownerStatus {
                        Image(systemName: Ink.symbol(status.icon)).font(.system(size: 11, weight: .semibold))
                    }
                    Text(OwnerToday.paymentLine(payment)).lineLimit(1)
                }
                .font(InkFont.footnote)
                .foregroundStyle(.white.opacity(0.85))
                .accessibilityElement(children: .combine)
            }
            HStack(spacing: 10) {
                Button("Подробнее", action: onOpen)
                    .buttonStyle(HeroButtonStyle())
                if let mail = mailURL {
                    Button("Написать") { openURL(mail) }
                        .buttonStyle(HeroButtonStyle())
                }
            }
            .padding(.top, 10)
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 26, style: .continuous)
                .fill(Ink.hero)
                .overlay(alignment: .topTrailing) {
                    // Мягкий круг-блик, как в макете.
                    Circle().fill(.white.opacity(0.1)).frame(width: 160, height: 160).offset(x: 50, y: -60)
                }
                .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
        )
        .accessibilityElement(children: .contain)
    }

    private var mailURL: URL? {
        let email = next.booking.guestEmail
        guard !email.isEmpty else { return nil }
        return URL(string: "mailto:\(email.addingPercentEncoding(withAllowedCharacters: .urlUserAllowed) ?? email)")
    }
}

/// Кнопки на акцентной карточке: полупрозрачные белые капсулы.
private struct HeroButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, minHeight: 40)
            .background(Capsule().fill(.white.opacity(configuration.isPressed ? 0.3 : 0.2)))
    }
}

// MARK: - Лента дня

struct DayTimeline: View {
    let entries: [DayEntry]
    let now: Date
    let onOpen: (Booking, String) -> Void
    let onToggleTask: (Int64) -> Void
    let offerText: (Date, Date, Int64) -> String

    private let timeWidth: CGFloat = 46
    private let capsule: CGFloat = 36

    var body: some View {
        VStack(spacing: 0) {
            ForEach(entries) { entry in
                row(entry)
            }
        }
        .padding(.vertical, 6)
        .background {
            // Пунктирная линия через центры капсул.
            GeometryReader { geo in
                let x = timeWidth + 10 + capsule / 2
                Path { path in
                    path.move(to: CGPoint(x: x, y: 24))
                    path.addLine(to: CGPoint(x: x, y: max(24, geo.size.height - 24)))
                }
                .stroke(Ink.tertiary, style: StrokeStyle(lineWidth: 1.5, dash: [2, 4]))
            }
        }
        .padding(.horizontal, 12)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Ink.card))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Лента дня")
    }

    @ViewBuilder
    private func row(_ entry: DayEntry) -> some View {
        switch entry {
        case .now(let date):
            HStack(spacing: 6) {
                Text(SelfvoyDate.time(date))
                    .font(.system(size: 11, weight: .bold).monospacedDigit())
                    .foregroundStyle(Ink.red)
                    .frame(width: timeWidth, alignment: .trailing)
                Circle().fill(Ink.red).frame(width: 8, height: 8)
                Rectangle().fill(Ink.red).frame(height: 1.5)
            }
            .padding(.vertical, 2)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Сейчас \(SelfvoyDate.time(date))")
        case .free(let start, let end, let pageId):
            HStack(spacing: 10) {
                time(start)
                Image(systemName: "plus")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Ink.secondary)
                    .frame(width: capsule, height: capsule)
                    .background(Circle().fill(Ink.card))
                    .overlay(Circle().strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [3, 3])).foregroundStyle(Ink.tertiary))
                VStack(alignment: .leading, spacing: 1) {
                    Text("Свободно").font(InkFont.headline).foregroundStyle(Ink.label)
                    Text("\(SelfvoyDate.time(start))–\(SelfvoyDate.time(end))").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                Spacer(minLength: 4)
                ShareLink(item: offerText(start, end, pageId)) {
                    Text("Предложить").font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(Ink.accent)
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .background(Capsule().fill(Ink.accentSoft))
                }
            }
            .padding(.vertical, 8)
        case .task(let task):
            HStack(spacing: 10) {
                time(task.at)
                Button {
                    onToggleTask(task.id)
                } label: {
                    Image(systemName: "checkmark")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(Ink.secondary)
                        .frame(width: capsule, height: capsule)
                        .background(Circle().fill(Ink.fill))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Выполнено: \(task.title)")
                VStack(alignment: .leading, spacing: 1) {
                    Text(task.title).font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                    Text("Задача").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                Spacer(minLength: 0)
            }
            .padding(.vertical, 8)
        case .booking(let booking, let occurrence, let start, let end, let color, let status):
            Button {
                onOpen(booking, occurrence.start)
            } label: {
                HStack(spacing: 10) {
                    time(start)
                    Image(systemName: color == 3 ? "star.fill" : "book.closed.fill")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: capsule, height: capsule)
                        .background(RoundedRectangle(cornerRadius: 11, style: .continuous).fill(Ink.capsule(color)))
                    VStack(alignment: .leading, spacing: 1) {
                        Text(booking.guestName.isEmpty ? "Гость" : booking.guestName)
                            .font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                        Text("\(booking.pageTitle.isEmpty ? "Запись" : booking.pageTitle) · \(Int(end.timeIntervalSince(start) / 60)) мин")
                            .font(InkFont.footnote).foregroundStyle(Ink.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 4)
                    StatusBadge(status: status, compact: true)
                }
                .padding(.vertical, 8)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        }
    }

    private func time(_ date: Date) -> some View {
        Text(SelfvoyDate.time(date))
            .font(.system(size: 13, weight: .semibold).monospacedDigit())
            .foregroundStyle(Ink.secondary)
            .frame(width: timeWidth, alignment: .trailing)
    }
}
