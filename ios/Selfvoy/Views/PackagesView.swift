// PackagesView.swift — «Абонементы» (F5): пакеты занятий клиентов с остатком,
// сроком и оплатой; выдать пакет вне сайта (package.issue — ссылка пакета
// показывается один раз), поправить остаток (package.adjust), отметить оплату
// (package.paymentSet) и отменить (package.cancel).
import SelfvoyKit
import SwiftUI
import UIKit

struct PackagesView: View {

    @EnvironmentObject private var store: AppStore
    @State private var filter = "open"
    @State private var selected: Int64?
    @State private var issuing = false

    private var shown: [OwnerPackage] {
        let list = store.packages ?? []
        switch filter {
        case "open": return list.filter { $0.status == "active" || $0.status == "pending" }
        case "ended": return list.filter { $0.status == "used" || $0.status == "expired" || $0.status == "cancelled" }
        default: return list
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Пакеты занятий", title: "Абонементы") {
                    InkRoundButton(systemImage: "chevron.left", label: "Назад") { store.selectTab(store.rootTab) }
                        .accessibilityIdentifier("nav.back")
                    InkRoundButton(systemImage: "plus", label: "Выдать абонемент") { issuing = true }
                }
                Picker("Показать", selection: $filter) {
                    Text("Действуют").tag("open")
                    Text("Закончились").tag("ended")
                    Text("Все").tag("all")
                }
                .pickerStyle(.segmented)
                if store.packages == nil {
                    InkEmpty(text: "Загружаем абонементы…")
                } else if shown.isEmpty {
                    InkEmpty(text: filter == "open"
                             ? "Действующих абонементов нет. Добавьте пакеты на страницу записи или выдайте абонемент вручную — кнопкой «+»."
                             : "Здесь пока пусто.")
                } else {
                    InkGroup {
                        ForEach(Array(shown.enumerated()), id: \.element.id) { index, item in
                            if index > 0 { InkDivider() }
                            PackageRow(package: item) { selected = item.id }
                        }
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .refreshable { store.loadPackages() }
        .onAppear {
            if store.issuePackageRequested {
                store.issuePackageRequested = false
                issuing = true
            }
        }
        .sheet(item: Binding(get: { selected.map(PackageID.init) }, set: { selected = $0?.id })) { target in
            PackageDetailSheet(packageId: target.id)
                .inkSheet()
        }
        .sheet(isPresented: $issuing) {
            IssuePackageSheet()
                .inkSheet(large: true)
        }
    }
}

private struct PackageID: Identifiable {
    let id: Int64
}

private struct PackageRow: View {
    let package: OwnerPackage
    let onOpen: () -> Void

    var body: some View {
        Button(action: onOpen) {
            HStack(spacing: 12) {
                InkAvatar(name: package.guestName, size: 40)
                VStack(alignment: .leading, spacing: 4) {
                    Text(package.guestName.isEmpty ? "Клиент" : package.guestName)
                        .font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                    Text("\(package.title) · \(package.remainingText)")
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary).lineLimit(1)
                    ProgressView(value: package.usedFraction)
                        .tint(package.sessionsLeft <= 1 ? Ink.amber : Ink.accent)
                        .frame(maxWidth: 140)
                }
                Spacer(minLength: 4)
                VStack(alignment: .trailing, spacing: 4) {
                    StatusBadge(status: package.statusView, compact: true)
                    if let pay = package.payment?.ownerStatus, package.payment?.awaiting == true {
                        StatusBadge(status: pay, compact: true)
                    }
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Карточка абонемента

private struct PackageDetailSheet: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    let packageId: Int64

    @State private var delta = 1
    @State private var busy = ""
    @State private var error = ""
    @State private var confirmCancel = false

    private var package: OwnerPackage? { store.packages?.first { $0.id == packageId } }

    var body: some View {
        ScrollView {
            if let package {
                VStack(alignment: .leading, spacing: 14) {
                    HStack(alignment: .top, spacing: 12) {
                        InkAvatar(name: package.guestName, size: 52)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(package.guestName.isEmpty ? "Клиент" : package.guestName)
                                .font(InkFont.title).foregroundStyle(Ink.label)
                            Text("\(package.title) · \(package.pageTitle)").font(InkFont.callout).foregroundStyle(Ink.secondary)
                        }
                        Spacer(minLength: 0)
                        StatusBadge(status: package.statusView)
                    }
                    summary(package)
                    if !error.isEmpty { InkNotice(text: error) }
                    if package.isOpen { adjust(package) }
                    payment(package)
                    contacts(package)
                    if package.isOpen {
                        Button("Отменить абонемент") { confirmCancel = true }
                            .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
                            .disabled(!busy.isEmpty)
                    }
                }
                .padding(20)
                .confirmationDialog("Отменить абонемент? По ссылке больше не записаться; сделанные записи останутся.",
                                    isPresented: $confirmCancel, titleVisibility: .visible) {
                    Button("Отменить абонемент", role: .destructive) {
                        run("cancel") { try await store.cancelPackage(package) }
                    }
                    Button("Не отменять", role: .cancel) {}
                }
            } else {
                Text("Абонемент не найден").font(InkFont.callout).foregroundStyle(Ink.secondary).padding(32)
            }
        }
        .background(Ink.bg.ignoresSafeArea())
    }

    private func summary(_ package: OwnerPackage) -> some View {
        InkGroup {
            row("Осталось занятий", "\(package.sessionsLeft) из \(package.sessionsTotal)")
            InkDivider(inset: 14)
            row("Действует до", package.validUntil.isEmpty ? "после оплаты, \(package.validDays) дн."
                                                           : SelfvoyDate.when(package.validUntil))
            InkDivider(inset: 14)
            row("Цена", package.price > 0 ? Money.format(package.price, currency: package.currency) : "бесплатно")
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).font(InkFont.callout).foregroundStyle(Ink.secondary)
            Spacer(minLength: 8)
            Text(value).font(InkFont.callout.monospacedDigit()).foregroundStyle(Ink.label)
        }
        .padding(14)
    }

    private func adjust(_ package: OwnerPackage) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Поправить остаток").font(InkFont.headline).foregroundStyle(Ink.label)
            Stepper(value: $delta, in: -min(100, package.sessionsLeft)...100) {
                Text(delta == 0 ? "Без изменений" : delta > 0 ? "Добавить \(delta)" : "Списать \(-delta)")
                    .font(InkFont.callout).foregroundStyle(Ink.label)
            }
            Button {
                let value = delta
                run("adjust") {
                    try await store.adjustPackage(package, delta: value)
                    delta = 1
                }
            } label: {
                if busy == "adjust" { ProgressView().tint(.white) } else { Text("Применить") }
            }
            .buttonStyle(InkPrimaryButtonStyle())
            .disabled(delta == 0 || !busy.isEmpty)
            Text("Например, занятие прошло вне записи или вы дарите дополнительное.")
                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
        }
        .inkCard()
    }

    @ViewBuilder
    private func payment(_ package: OwnerPackage) -> some View {
        if let payment = package.payment {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("Оплата \(payment.amountText)").font(InkFont.headline).foregroundStyle(Ink.label)
                    Spacer()
                    if let status = payment.ownerStatus { StatusBadge(status: status) }
                }
                ForEach(payment.ownerActions, id: \.self) { mark in
                    Button {
                        run("pay-\(mark.rawValue)") { try await store.setPackagePayment(package, mark) }
                    } label: {
                        if busy == "pay-\(mark.rawValue)" { ProgressView() } else { Text(mark.title) }
                    }
                    .buttonStyle(InkSecondaryButtonStyle(tint: mark == .refunded ? Ink.red : Ink.accent))
                    .disabled(!busy.isEmpty)
                }
            }
            .inkCard()
        }
    }

    @ViewBuilder
    private func contacts(_ package: OwnerPackage) -> some View {
        if !package.guestEmail.isEmpty || !package.ccEmail.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                if !package.guestEmail.isEmpty {
                    Button {
                        if let url = URL(string: "mailto:\(package.guestEmail)") { openURL(url) }
                    } label: {
                        Label(package.guestEmail, systemImage: "envelope")
                    }
                }
                if !package.ccEmail.isEmpty {
                    Text("Копия писем: \(package.ccEmail)").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
            }
            .font(InkFont.callout)
            .foregroundStyle(Ink.accent)
            .inkCard()
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

// MARK: - Выдать абонемент

private struct IssuePackageSheet: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss

    @State private var pageId: Int64 = 0
    /// 0 — свой пакет; иначе id предложения страницы.
    @State private var offerId: Int64 = 0
    @State private var title = ""
    @State private var sessions = 8
    @State private var priceText = ""
    @State private var validDays = 90
    @State private var guestName = ""
    @State private var guestEmail = ""
    @State private var ccEmail = ""
    @State private var paid = false
    @State private var error = ""
    @State private var saving = false
    @State private var result: PackageIssueResult?

    private var pages: [BookingPage] { store.bookingPages ?? [] }
    private var page: BookingPage? { pages.first { $0.id == pageId } }

    var body: some View {
        NavigationStack {
            Group {
                if let result {
                    issued(result)
                } else {
                    form
                }
            }
            .navigationTitle(result == nil ? "Выдать абонемент" : "Абонемент выдан")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                if result == nil {
                    ToolbarItem(placement: .cancellationAction) { Button("Отмена") { dismiss() } }
                    ToolbarItem(placement: .confirmationAction) {
                        if saving { ProgressView() } else { Button("Выдать") { issue() }.bold() }
                    }
                } else {
                    ToolbarItem(placement: .confirmationAction) { Button("Готово") { dismiss() }.bold() }
                }
            }
        }
        .tint(Ink.accent)
        .onAppear {
            if pageId == 0, let first = pages.first { pageId = first.id }
        }
    }

    private var form: some View {
        Form {
            Section("Страница и пакет") {
                if pages.isEmpty {
                    Text("Сначала создайте страницу записи.").foregroundStyle(Ink.secondary)
                } else {
                    Picker("Страница", selection: $pageId) {
                        ForEach(pages) { item in Text(item.fields.title).tag(item.id) }
                    }
                    Picker("Пакет", selection: $offerId) {
                        Text("Свой пакет").tag(Int64(0))
                        ForEach(page?.fields.packages ?? []) { offer in
                            Text("\(offer.title) · \(Money.format(offer.price, currency: page?.fields.currency ?? "RUB"))").tag(offer.id)
                        }
                    }
                    if offerId == 0 {
                        TextField("Название, например «8 уроков»", text: $title)
                        Stepper("Занятий: \(sessions)", value: $sessions, in: 1...100)
                        HStack {
                            Text("Цена")
                            Spacer()
                            TextField("0", text: $priceText)
                                .keyboardType(.decimalPad)
                                .multilineTextAlignment(.trailing)
                                .frame(maxWidth: 140)
                            Text(Money.symbol(page?.fields.currency ?? "RUB")).foregroundStyle(Ink.secondary)
                        }
                        Picker("Действует", selection: $validDays) {
                            ForEach([30, 60, 90, 180, 365], id: \.self) { days in
                                Text("\(days) \(Format.plural(days, "день", "дня", "дней"))").tag(days)
                            }
                        }
                    }
                }
            }
            Section {
                TextField("Имя клиента", text: $guestName)
                TextField("Email клиента — пришлём ссылку", text: $guestEmail)
                    .keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField("Копия писем, например родителю", text: $ccEmail)
                    .keyboardType(.emailAddress).textInputAutocapitalization(.never).autocorrectionDisabled()
            } header: {
                Text("Клиент")
            }
            Section {
                Toggle("Уже оплачен", isOn: $paid)
            } footer: {
                Text(paid ? "Оплата отмечена сразу." : "Оплату отметите позже в карточке абонемента.")
            }
            if !error.isEmpty {
                Section { InkNotice(text: error).listRowInsets(EdgeInsets()).listRowBackground(Color.clear) }
            }
        }
        .onChange(of: pageId) { _ in offerId = 0 }
    }

    private func issued(_ result: PackageIssueResult) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkNotice(text: "Ссылка пакета показывается один раз. Отправьте её клиенту сейчас — по ней он записывается по абонементу.",
                          tone: .amber)
                VStack(alignment: .leading, spacing: 6) {
                    Text("\(result.package.guestName) · \(result.package.title)").font(InkFont.headline).foregroundStyle(Ink.label)
                    Text(result.package.remainingText).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                    Text(result.link).font(InkFont.footnote.monospaced()).foregroundStyle(Ink.label)
                        .textSelection(.enabled).padding(.top, 4)
                    if result.url.isEmpty {
                        Text("Сервер не знает свой публичный адрес — допишите адрес веб-клиента перед ссылкой.")
                            .font(InkFont.caption).foregroundStyle(Ink.secondary)
                    }
                }
                .inkCard()
                ShareLink(item: "Ваш абонемент «\(result.package.title)»: \(result.link)") {
                    Label("Отправить клиенту", systemImage: "square.and.arrow.up")
                }
                .buttonStyle(InkPrimaryButtonStyle())
                Button {
                    UIPasteboard.general.string = result.link
                    store.setStatus("Ссылка скопирована")
                } label: {
                    Label("Скопировать ссылку", systemImage: "doc.on.doc").frame(maxWidth: .infinity)
                }
                .buttonStyle(InkSecondaryButtonStyle())
            }
            .padding(20)
        }
        .background(Ink.bg)
    }

    private func issue() {
        guard let price = Money.parse(priceText) else {
            error = "Проверьте цену"
            return
        }
        let request = PackageIssueRequest(pageId: pageId, offerId: offerId == 0 ? nil : offerId, title: title,
                                          sessions: sessions, price: price, validDays: validDays,
                                          guestName: guestName, guestEmail: guestEmail, ccEmail: ccEmail, paid: paid)
        if let problem = request.validate() {
            error = problem
            return
        }
        error = ""
        saving = true
        Task { @MainActor in
            do {
                result = try await store.issuePackage(request)
            } catch {
                self.error = store.describe(error)
            }
            saving = false
        }
    }
}
