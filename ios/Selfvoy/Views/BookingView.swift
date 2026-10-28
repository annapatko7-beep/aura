// BookingView.swift — «Запись» (дизайн 2026): страницы записи владельца
// (booking.*), постоянная ссылка #/b/<slug> (страница гостя — в веб-клиенте),
// настройки страницы (PageSettingsView), «Пост со свободными окнами»
// (booking.slotsPost) и записи гостей; нажатие на запись — карточка записи.
import SelfvoyKit
import SwiftUI
import UIKit

struct BookingView: View {

    @EnvironmentObject private var store: AppStore
    /// nil — окно настроек закрыто; .new — новая страница; .edit — правка.
    @State private var editor: PageEditorTarget?
    @State private var postPage: BookingPage?
    @State private var focus: BookingFocus?
    @State private var showPast = false

    private var upcoming: [Booking] {
        let now = Date()
        return store.bookings.filter { booking in
            booking.isActive && booking.occurrences.contains { item in
                item.active && (SelfvoyDate.parse(item.end.isEmpty ? item.start : item.end).map { $0 > now } ?? false)
            }
        }
        .sorted { nextStart($0) < nextStart($1) }
    }

    private var past: [Booking] {
        let ids = Set(upcoming.map(\.id))
        return store.bookings.filter { !ids.contains($0.id) }
    }

    private func nextStart(_ booking: Booking) -> String {
        let now = Date()
        return booking.occurrences.first { item in
            item.active && (SelfvoyDate.parse(item.start).map { $0 > now } ?? false)
        }?.start ?? booking.occurrences.last?.start ?? ""
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Страницы и записи", title: "Запись") {
                    InkRoundButton(systemImage: "plus", label: "Новая страница") { editor = .new }
                }
                pagesSection
                InkSectionHeader(title: "Записи", trailing: upcoming.isEmpty ? "" : "\(upcoming.count) предстоит")
                if !store.bookingsLoaded {
                    InkEmpty(text: "Загружаем записи…")
                } else if upcoming.isEmpty {
                    InkEmpty(text: "Здесь появятся люди, которые записались по вашим ссылкам.")
                } else {
                    InkGroup {
                        ForEach(Array(upcoming.enumerated()), id: \.element.id) { index, booking in
                            if index > 0 { InkDivider() }
                            BookingRow(booking: booking) { focus = BookingFocus(bookingId: booking.id, start: nil) }
                        }
                    }
                }
                if !past.isEmpty {
                    Button(showPast ? "Скрыть прошедшие и отменённые" : "Прошедшие и отменённые: \(past.count)") {
                        withAnimation { showPast.toggle() }
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                    if showPast {
                        InkGroup {
                            ForEach(Array(past.prefix(50).enumerated()), id: \.element.id) { index, booking in
                                if index > 0 { InkDivider() }
                                BookingRow(booking: booking) { focus = BookingFocus(bookingId: booking.id, start: nil) }
                            }
                        }
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .refreshable { store.loadBooking() }
        .onAppear {
            store.loadBooking()
            consumeIntent()
        }
        .onChange(of: store.bookingIntent) { _ in consumeIntent() }
        .sheet(item: $editor) { target in
            PageSettingsView(page: target.page)
                .inkSheet(large: true)
        }
        .sheet(item: $postPage) { page in
            SlotsPostSheet(page: page)
                .inkSheet()
        }
        .sheet(item: $focus) { target in
            BookingSheetView(focus: target)
                .inkSheet()
        }
    }

    /// Запрос из «+» или «Сегодня»: новая страница или пост.
    private func consumeIntent() {
        guard let intent = store.bookingIntent else { return }
        store.bookingIntent = nil
        switch intent {
        case .newPage:
            editor = .new
        case .post:
            if let page = (store.bookingPages ?? []).first(where: { $0.isActive }) {
                postPage = page
            } else {
                store.errorMessage = "Сначала откройте страницу записи — пост собирается из её свободных окон"
            }
        }
    }

    @ViewBuilder
    private var pagesSection: some View {
        if let pages = store.bookingPages {
            if pages.isEmpty {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Постоянная ссылка для клиентов").font(InkFont.headline).foregroundStyle(Ink.label)
                    Text("Люди без Selfvoy сами выбирают свободное время в вашем расписании — можно сразу серией раз в неделю, с ценой, предоплатой и напоминаниями.")
                        .font(InkFont.callout).foregroundStyle(Ink.secondary)
                    Button("Создать страницу") { editor = .new }
                        .buttonStyle(InkPrimaryButtonStyle())
                        .padding(.top, 4)
                }
                .inkCard(padding: 18)
            }
            ForEach(pages) { page in
                PageCard(page: page,
                         onEdit: { editor = .edit(page) },
                         onPost: { postPage = page })
            }
        } else {
            InkEmpty(text: "Загружаем страницы…")
        }
    }
}

/// Цель окна настроек страницы.
enum PageEditorTarget: Identifiable {
    case new
    case edit(BookingPage)

    var id: String {
        switch self {
        case .new: return "new"
        case .edit(let page): return "page-\(page.id)"
        }
    }

    var page: BookingPage? {
        if case .edit(let page) = self { return page }
        return nil
    }
}

// MARK: - Карточка страницы

private struct PageCard: View {

    @EnvironmentObject private var store: AppStore
    let page: BookingPage
    let onEdit: () -> Void
    let onPost: () -> Void
    @State private var confirmDelete = false

    private var facts: String {
        let f = page.fields
        var parts = ["\(f.durationMinutes) мин"]
        if f.price > 0 { parts.append(Money.format(f.price, currency: f.currency)) }
        parts.append("на \(f.daysAhead) \(Format.plural(f.daysAhead, "день", "дня", "дней")) вперёд")
        parts.append("до \(f.maxPerDay) в день")
        if f.maxSeries > 1 { parts.append("серия до \(f.maxSeries)") }
        return parts.joined(separator: " · ")
    }

    private var extras: [String] {
        let f = page.fields
        var list: [String] = []
        switch f.prepay {
        case .full: list.append("предоплата")
        case .deposit: list.append("депозит \(Money.format(f.deposit, currency: f.currency))")
        case .off: break
        }
        if f.cancelHours > 0 { list.append("отмена за \(f.cancelHours) ч") }
        if f.guestReminders { list.append("напоминания") }
        if !f.packages.isEmpty { list.append("пакетов: \(f.packages.count)") }
        if f.hideBranding { list.append("без подписи") }
        return list
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "calendar.badge.clock")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 38, height: 38)
                    .background(RoundedRectangle(cornerRadius: 11, style: .continuous)
                        .fill(Ink.capsule(OwnerToday.pageColor(page.id, pages: store.bookingPages ?? []))))
                VStack(alignment: .leading, spacing: 2) {
                    Text(page.fields.title).font(InkFont.headline).foregroundStyle(Ink.label)
                    Text(facts).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                Spacer(minLength: 4)
                StatusBadge(status: page.isActive ? StatusView("Открыта", .green, .check)
                                                  : StatusView("На паузе", .amber, .clock), compact: true)
            }
            if !extras.isEmpty {
                Text(extras.joined(separator: " · ")).font(InkFont.footnote).foregroundStyle(Ink.secondary)
            }
            HStack(spacing: 8) {
                Text(page.link).font(InkFont.footnote.monospaced()).foregroundStyle(Ink.label)
                    .lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                Spacer(minLength: 4)
                Button {
                    UIPasteboard.general.string = page.link
                    store.setStatus("Ссылка скопирована")
                } label: {
                    Image(systemName: "doc.on.doc").font(.system(size: 14, weight: .semibold))
                }
                .accessibilityLabel("Копировать ссылку")
                ShareLink(item: page.link) {
                    Image(systemName: "square.and.arrow.up").font(.system(size: 14, weight: .semibold))
                }
                .accessibilityLabel("Поделиться ссылкой")
            }
            .foregroundStyle(Ink.accent)
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Ink.fill))
            if page.url.isEmpty {
                Text("Сервер не знает свой публичный адрес (SELFVOY_PUBLIC_URL) — полную ссылку соберите из адреса веб-клиента.")
                    .font(InkFont.caption).foregroundStyle(Ink.secondary)
            }
            Text(page.upcoming > 0
                 ? "Предстоит: \(page.upcoming) \(Format.plural(page.upcoming, "встреча", "встречи", "встреч"))"
                 : "Пока никто не записался")
                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    Button("Настроить", action: onEdit).buttonStyle(InkSecondaryButtonStyle())
                    if page.isActive {
                        Button("Пост со свободными окнами", action: onPost).buttonStyle(InkSecondaryButtonStyle(tint: Ink.amberInk))
                        Button("Пауза") { store.setBookingPagePaused(page, paused: true) }
                            .buttonStyle(InkSecondaryButtonStyle(tint: Ink.secondary))
                    } else {
                        Button("Открыть запись") { store.setBookingPagePaused(page, paused: false) }
                            .buttonStyle(InkSecondaryButtonStyle(tint: Ink.green))
                    }
                    Button("Удалить") { confirmDelete = true }
                        .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
                }
            }
        }
        .inkCard()
        .confirmationDialog("Удалить страницу «\(page.fields.title)»? Ссылка перестанет работать, сделанные записи останутся.",
                            isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Удалить страницу", role: .destructive) { store.deleteBookingPage(page) }
            Button("Не удалять", role: .cancel) {}
        }
    }
}

// MARK: - Строка записи

private struct BookingRow: View {
    let booking: Booking
    let onOpen: () -> Void

    private var status: StatusView {
        let now = Date()
        if !booking.isActive {
            switch booking.cancelledBy {
            case "owner": return StatusView("Отменили вы", .red, .cross)
            case "system": return StatusView("Не оплачено", .red, .cross)
            default: return StatusView("Отменил гость", .red, .cross)
            }
        }
        if let next = OwnerToday.focusOccurrence(booking, start: nil, now: now) {
            return OwnerToday.occurrenceStatus(booking, next, now: now)
        }
        return StatusView("Прошла", .neutral, .check)
    }

    private var when: String {
        let now = Date()
        let shown = booking.occurrences.filter { item in
            item.active && (SelfvoyDate.parse(item.start).map { $0 > now } ?? false)
        }
        let list = shown.isEmpty ? Array(booking.occurrences.suffix(1)) : Array(shown.prefix(2))
        var text = list.map { SelfvoyDate.when($0.start) }.joined(separator: " · ")
        if shown.count > 2 { text += " · ещё \(shown.count - 2)" }
        return text
    }

    var body: some View {
        Button(action: onOpen) {
            HStack(spacing: 12) {
                InkAvatar(name: booking.guestName, size: 40)
                VStack(alignment: .leading, spacing: 2) {
                    Text(booking.guestName.isEmpty ? "Гость" : booking.guestName)
                        .font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                    Text("\(booking.pageTitle) · \(when)").font(InkFont.footnote).foregroundStyle(Ink.secondary).lineLimit(1)
                }
                Spacer(minLength: 4)
                StatusBadge(status: status, compact: true)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}
