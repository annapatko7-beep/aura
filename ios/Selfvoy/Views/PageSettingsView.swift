// PageSettingsView.swift — «Страница записи»: все поля страницы (дизайн 2026,
// сгруппированный список как в макете): услуга и цена, расписание, оплата и
// отмена (F4: предоплата/депозит, ссылка и реквизиты, срок оплаты, бесплатная
// отмена, поздняя отмена, условия), напоминания клиенту (F1), пакеты занятий (F5)
// и подпись «Запись через Selfvoy» (F3). Правка уходит только изменёнными полями.
import SelfvoyKit
import SwiftUI

struct PageSettingsView: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss
    /// nil — новая страница.
    let page: BookingPage?

    @State private var fields = BookingPageFields()
    @State private var priceText = ""
    @State private var depositText = ""
    @State private var error = ""
    @State private var saving = false
    @State private var loaded = false
    @State private var editingOffer: OfferDraft?

    private let durations = [15, 30, 45, 50, 60, 90, 120, 180]
    private let horizons = [7, 14, 30, 60]
    private let payWindows = [15, 30, 60, 120, 180, 360, 720, 1440]
    private let cancelWindows = [0, 2, 4, 6, 12, 24, 48, 72]

    /// Доступны ли функции Pro (иначе показываем подсказку — сервер всё равно проверит).
    private var proOpen: Bool { store.userPlan?.proFeaturesOpen() ?? true }

    var body: some View {
        NavigationStack {
            Form {
                serviceSection
                scheduleSection
                paymentSection
                cancelSection
                remindersSection
                packagesSection
                brandingSection
                if !error.isEmpty {
                    Section { InkNotice(text: error).listRowInsets(EdgeInsets()).listRowBackground(Color.clear) }
                }
            }
            .navigationTitle(page == nil ? "Новая страница" : "Страница записи")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Отмена") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if saving {
                        ProgressView()
                    } else {
                        Button(page == nil ? "Создать" : "Готово") { save() }.bold()
                    }
                }
            }
            .sheet(item: $editingOffer) { draft in
                OfferEditor(draft: draft, currency: fields.currency) { offer in
                    if let index = fields.packages.firstIndex(where: { $0.id == offer.id && offer.id != 0 }) {
                        fields.packages[index] = offer
                    } else if draft.index < fields.packages.count {
                        fields.packages[draft.index] = offer
                    } else {
                        fields.packages.append(offer)
                    }
                }
                .inkSheet()
            }
        }
        .tint(Ink.accent)
        .onAppear(perform: load)
    }

    private func load() {
        guard !loaded else { return }
        loaded = true
        if let page { fields = page.fields }
        priceText = Money.inputValue(fields.price)
        depositText = Money.inputValue(fields.deposit)
        if store.payments == nil { store.loadGrowthIntegrations() }
    }

    private var currencySymbol: String { Money.symbol(fields.currency) }

    // MARK: Услуга

    private var serviceSection: some View {
        Section("Услуга") {
            TextField("Название, например «Урок математики»", text: $fields.title)
            TextField("Описание для клиентов", text: $fields.description, axis: .vertical)
                .lineLimit(2...6)
            Picker("Длительность", selection: $fields.durationMinutes) {
                ForEach(durationOptions, id: \.self) { minutes in
                    Text(durationLabel(minutes)).tag(minutes)
                }
            }
            HStack {
                Text("Цена")
                Spacer()
                TextField("0", text: $priceText)
                    .keyboardType(.decimalPad)
                    .multilineTextAlignment(.trailing)
                    .frame(maxWidth: 140)
                Text(currencySymbol).foregroundStyle(Ink.secondary)
            }
            Picker("Валюта", selection: $fields.currency) {
                ForEach(Money.currencies, id: \.code) { currency in
                    Text(currency.title).tag(currency.code)
                }
            }
        }
    }

    private var durationOptions: [Int] {
        durations.contains(fields.durationMinutes) ? durations : (durations + [fields.durationMinutes]).sorted()
    }

    private func durationLabel(_ minutes: Int) -> String {
        if minutes < 60 || minutes % 30 != 0 { return "\(minutes) мин" }
        if minutes == 60 { return "1 час" }
        if minutes == 90 { return "1,5 часа" }
        return minutes % 60 == 0 ? "\(minutes / 60) часа" : "\(minutes) мин"
    }

    // MARK: Расписание

    private var scheduleSection: some View {
        Section {
            Picker("Запись открыта на", selection: $fields.daysAhead) {
                ForEach(horizons.contains(fields.daysAhead) ? horizons : (horizons + [fields.daysAhead]).sorted(),
                        id: \.self) { days in
                    Text(horizonLabel(days)).tag(days)
                }
            }
            Stepper("Встреч в день: до \(fields.maxPerDay)", value: $fields.maxPerDay, in: 1...16)
            Stepper("Запас до встречи: \(fields.noticeHours) ч", value: $fields.noticeHours, in: 0...168)
            Stepper(fields.maxSeries > 1 ? "Серия: до \(fields.maxSeries) встреч раз в неделю" : "Серия: только одна встреча",
                    value: $fields.maxSeries, in: 1...12)
            Toggle("Выходные", isOn: $fields.weekends)
        } header: {
            Text("Расписание")
        } footer: {
            Text(fields.weekends ? "Клиенты видят будни и выходные." : "Клиенты видят только будни.")
        }
    }

    private func horizonLabel(_ days: Int) -> String {
        switch days {
        case 7: return "неделю"
        case 14: return "2 недели"
        case 30: return "месяц"
        case 60: return "2 месяца"
        default: return "\(days) \(Format.plural(days, "день", "дня", "дней"))"
        }
    }

    // MARK: Оплата

    private var paymentSection: some View {
        Section {
            Picker("Предоплата", selection: $fields.prepay) {
                ForEach(BookingPrepay.allCases, id: \.self) { mode in
                    Text(mode.title).tag(mode)
                }
            }
            .pickerStyle(.segmented)
            if fields.prepay == .deposit {
                HStack {
                    Text("Депозит")
                    Spacer()
                    TextField("0", text: $depositText)
                        .keyboardType(.decimalPad)
                        .multilineTextAlignment(.trailing)
                        .frame(maxWidth: 140)
                    Text(currencySymbol).foregroundStyle(Ink.secondary)
                }
            }
            if fields.prepay != .off {
                Picker("Время держится до оплаты", selection: $fields.payWithinMinutes) {
                    ForEach(payWindows.contains(fields.payWithinMinutes) ? payWindows
                            : (payWindows + [fields.payWithinMinutes]).sorted(), id: \.self) { minutes in
                        Text(minutes < 60 ? "\(minutes) мин" : minutes == 1440 ? "сутки" : "\(minutes / 60) ч").tag(minutes)
                    }
                }
            }
            HStack(spacing: 10) {
                Image(systemName: "creditcard.fill").foregroundStyle(Ink.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text("ЮKassa")
                    Text(store.payments?.connected == true ? "Подключена: оплата картой и через СБП"
                                                          : "Не подключена — Ещё → Подключения")
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                Spacer()
                StatusBadge(status: store.payments?.connected == true ? StatusView("Подключена", .green, .check)
                                                                     : StatusView("Нет", .neutral, .minus), compact: true)
            }
            TextField("Ссылка оплаты https://…", text: $fields.paymentUrl)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            TextField("Реквизиты или СБП — одной строкой", text: $fields.paymentNote)
        } header: {
            Text("Оплата")
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                Text(prepayFooter)
                if fields.prepay != .off && !proOpen {
                    Text("Предоплата — на тарифе Pro.").foregroundStyle(Ink.amberInk)
                }
            }
        }
    }

    private var prepayFooter: String {
        switch fields.prepay {
        case .off: return "Цена видна клиенту; платят на встрече. Цена без предоплаты — бесплатно."
        case .full: return "Клиент оплачивает всю сумму сразу; не оплатил вовремя — запись отменится сама."
        case .deposit: return "Клиент вносит депозит сразу; остаток — на встрече."
        }
    }

    private var cancelSection: some View {
        Section {
            Picker("Бесплатная отмена", selection: $fields.cancelHours) {
                ForEach(cancelWindows.contains(fields.cancelHours) ? cancelWindows
                        : (cancelWindows + [fields.cancelHours]).sorted(), id: \.self) { hours in
                    Text(hours == 0 ? "в любое время" : "за \(hours) ч").tag(hours)
                }
            }
            if fields.cancelHours > 0 {
                Picker("Позже срока", selection: $fields.lateCancel) {
                    Text(fields.prepay == .off ? "можно отменить" : "отмена без возврата").tag(BookingLateCancel.allow)
                    Text("только по договорённости").tag(BookingLateCancel.block)
                }
            }
            TextField("Условия отмены для клиента", text: $fields.policyText, axis: .vertical)
                .lineLimit(1...4)
        } header: {
            Text("Отмена и перенос")
        } footer: {
            Text(fields.cancelHours == 0
                 ? "Клиент может перенести или отменить встречу в любое время."
                 : fields.lateCancel == .block
                    ? "Позже чем за \(fields.cancelHours) ч клиент не сможет сам отменить или перенести встречу."
                    : "Позже чем за \(fields.cancelHours) ч отменить можно, но оплата не возвращается.")
        }
    }

    // MARK: Напоминания, пакеты, подпись

    private var remindersSection: some View {
        Section {
            Toggle("Напоминать клиенту", isOn: $fields.guestReminders)
        } header: {
            Text("Напоминания клиенту")
        } footer: {
            Text("За сутки и за 2 часа — письмом и в Telegram, с кнопкой «Приду». Бесплатно.")
        }
    }

    private var packagesSection: some View {
        Section {
            ForEach(Array(fields.packages.enumerated()), id: \.offset) { index, offer in
                Button {
                    editingOffer = OfferDraft(index: index, offer: offer)
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(offer.title).foregroundStyle(Ink.label)
                            Text("\(offer.sessions) \(Format.plural(offer.sessions, "занятие", "занятия", "занятий")) · \(offer.validDays) дн.")
                                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                        }
                        Spacer()
                        Text(Money.format(offer.price, currency: fields.currency)).foregroundStyle(Ink.secondary)
                    }
                }
            }
            .onDelete { offsets in fields.packages.remove(atOffsets: offsets) }
            if fields.packages.count < PackageOffer.maxPerPage {
                Button {
                    editingOffer = OfferDraft(index: fields.packages.count, offer: PackageOffer(title: "", sessions: 8,
                                                                                            price: fields.price * 8, validDays: 90))
                } label: {
                    Label("Добавить пакет", systemImage: "plus.circle.fill")
                }
            }
        } header: {
            Text("Пакеты занятий")
        } footer: {
            Text("Абонемент на несколько встреч со своим сроком. Клиент покупает его при записи; выдать вручную — в «Абонементах»."
                 + (proOpen ? "" : " Пакеты — на тарифе Pro."))
        }
    }

    private var brandingSection: some View {
        Section {
            Toggle("Скрыть «Запись через Selfvoy»", isOn: $fields.hideBranding)
        } header: {
            HStack(spacing: 6) {
                Text("Подпись")
                Text("PRO").font(.system(size: 10, weight: .heavy)).foregroundStyle(.white)
                    .padding(.horizontal, 5).padding(.vertical, 1)
                    .background(Capsule().fill(Ink.accent))
            }
        } footer: {
            Text(proOpen ? "Подпись внизу страницы записи и в письмах клиенту."
                         : "Скрыть подпись можно на тарифе Pro. Пригласите коллегу — вы оба получите месяц Pro.")
        }
    }

    // MARK: Сохранение

    private func save() {
        guard let price = Money.parse(priceText) else {
            error = "Проверьте цену: например, 2000 или 2000,50"
            return
        }
        guard let deposit = Money.parse(depositText) else {
            error = "Проверьте депозит"
            return
        }
        var draft = fields
        draft.price = price
        draft.deposit = fields.prepay == .deposit ? deposit : 0
        if let problem = draft.validate(paymentsConnected: store.payments?.connected) {
            error = problem
            return
        }
        error = ""
        saving = true
        Task { @MainActor in
            do {
                try await store.saveBookingPage(page: page, fields: draft)
                saving = false
                dismiss()
            } catch {
                saving = false
                self.error = store.describe(error)
            }
        }
    }
}

// MARK: - Пакет на странице

struct OfferDraft: Identifiable {
    let index: Int
    let offer: PackageOffer
    var id: Int { index }
}

private struct OfferEditor: View {
    @Environment(\.dismiss) private var dismiss
    let draft: OfferDraft
    let currency: String
    let onSave: (PackageOffer) -> Void

    @State private var offer = PackageOffer()
    @State private var priceText = ""
    @State private var error = ""
    @State private var loaded = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Название, например «8 уроков»", text: $offer.title)
                    Stepper("Занятий: \(offer.sessions)", value: $offer.sessions, in: 2...100)
                    HStack {
                        Text("Цена пакета")
                        Spacer()
                        TextField("0", text: $priceText)
                            .keyboardType(.decimalPad)
                            .multilineTextAlignment(.trailing)
                            .frame(maxWidth: 140)
                        Text(Money.symbol(currency)).foregroundStyle(Ink.secondary)
                    }
                    Picker("Действует", selection: $offer.validDays) {
                        ForEach([30, 60, 90, 180, 365].contains(offer.validDays) ? [30, 60, 90, 180, 365]
                                : ([30, 60, 90, 180, 365] + [offer.validDays]).sorted(), id: \.self) { days in
                            Text("\(days) \(Format.plural(days, "день", "дня", "дней"))").tag(days)
                        }
                    }
                } footer: {
                    Text("Срок считается с момента оплаты.")
                }
                if !error.isEmpty {
                    Section { InkNotice(text: error).listRowInsets(EdgeInsets()).listRowBackground(Color.clear) }
                }
            }
            .navigationTitle(draft.offer.title.isEmpty ? "Новый пакет" : "Пакет")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Отмена") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Готово") {
                        guard let price = Money.parse(priceText) else {
                            error = "Проверьте цену"
                            return
                        }
                        var result = offer
                        result.price = price
                        if let problem = result.validate() {
                            error = problem
                            return
                        }
                        onSave(result)
                        dismiss()
                    }
                    .bold()
                }
            }
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            offer = draft.offer
            priceText = Money.inputValue(draft.offer.price)
        }
    }
}
