// IntegrationsView.swift — «Подключения» (дизайн 2026): Telegram-бот (F2),
// ЮKassa (F4) и подписка на календарь (ICS, F6).
//
// Секретный ключ ЮKassa вводится в SecureField, живёт только в @State этого
// экрана и уходит одним запросом payments.connect; после попытки поле очищается.
// Его не пишем ни в Keychain, ни в UserDefaults, ни в лог.
import SelfvoyKit
import SwiftUI
import UIKit

struct IntegrationsView: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.openURL) private var openURL

    @State private var shopId = ""
    @State private var secretKey = ""
    @State private var connecting = false
    @State private var paymentsError = ""
    @State private var telegramBusy = false
    @State private var telegramError = ""
    @State private var feedBusy = false
    @State private var feedError = ""
    @State private var confirmReset = false
    @State private var confirmDisableFeed = false
    @State private var confirmDisconnect = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Telegram, оплата, календарь", title: "Подключения") {
                    InkRoundButton(systemImage: "chevron.left", label: "Назад") { store.selectTab(store.rootTab) }
                        .accessibilityIdentifier("nav.back")
                }
                telegramCard
                paymentsCard
                calendarCard
                Button {
                    store.selectTab(.settings)
                } label: {
                    Label("Google Календарь и Gmail — в настройках", systemImage: "gearshape")
                        .font(InkFont.callout)
                }
                .buttonStyle(InkSecondaryButtonStyle(tint: Ink.secondary))
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .refreshable { store.loadGrowthIntegrations() }
        .onDisappear {
            // Секрет не переживает уход с экрана.
            secretKey = ""
        }
    }

    // MARK: Telegram

    private var telegramCard: some View {
        let status = store.telegram
        return VStack(alignment: .leading, spacing: 12) {
            cardHeader("Telegram", symbol: "paperplane.fill", tint: Color(hex: 0x229ED9),
                       status: status.map { $0.linked ? StatusView("Подключён", .green, .check)
                                                       : $0.enabled ? StatusView("Не подключён", .neutral, .minus)
                                                                    : StatusView("Недоступен", .neutral, .minus) })
            Text("Новые записи, переносы, отмены, «Приду» и оплаты — сообщением в Telegram. Отключить можно и командой /stop в чате с ботом.")
                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
            if let status {
                Text(status.summary + (status.enabled && !status.botUsername.isEmpty ? " · бот @\(status.botUsername)" : ""))
                    .font(InkFont.callout).foregroundStyle(Ink.label)
                if status.linked {
                    Toggle("Дублировать уведомления в Telegram", isOn: Binding(
                        get: { store.telegramNotifications },
                        set: { store.setTelegramNotifications($0) }))
                        .font(InkFont.callout)
                    Button("Отключить Telegram") { store.telegramUnlink() }
                        .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
                } else if status.enabled {
                    Button {
                        connectTelegram()
                    } label: {
                        if telegramBusy { ProgressView().tint(.white) } else { Text("Подключить") }
                    }
                    .buttonStyle(InkPrimaryButtonStyle())
                    .disabled(telegramBusy)
                    Text("Откроется Telegram: нажмите «Старт» в чате с ботом. Ссылка действует 15 минут, затем обновите экран.")
                        .font(InkFont.caption).foregroundStyle(Ink.secondary)
                }
            } else {
                Text("Проверяем…").font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
            if !telegramError.isEmpty { InkNotice(text: telegramError) }
        }
        .inkCard()
    }

    private func connectTelegram() {
        telegramBusy = true
        telegramError = ""
        Task { @MainActor in
            do {
                openURL(try await store.telegramConnectURL())
            } catch {
                telegramError = store.describe(error)
            }
            telegramBusy = false
        }
    }

    // MARK: ЮKassa

    private var paymentsCard: some View {
        let status = store.payments
        return VStack(alignment: .leading, spacing: 12) {
            cardHeader("ЮKassa", symbol: "creditcard.fill", tint: Ink.accent,
                       status: status.map { $0.connected ? StatusView("Подключена", .green, .check)
                                                         : StatusView("Не подключена", .neutral, .minus) })
            Text("Свой магазин: деньги клиентов идут вам напрямую, Selfvoy их не касается. Предоплата по ссылке и реквизитам работает и без ЮKassa.")
                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
            if let status, status.connected {
                Text("Магазин \(status.shopId)" + (status.connectedAt.isEmpty ? "" : " · с \(SelfvoyDate.when(status.connectedAt))"))
                    .font(InkFont.callout).foregroundStyle(Ink.label)
                Button("Отключить ЮKassa") { confirmDisconnect = true }
                    .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
            } else if status != nil {
                VStack(spacing: 0) {
                    TextField("shopId", text: $shopId)
                        .keyboardType(.numberPad)
                        .padding(12)
                    InkDivider(inset: 12)
                    SecureField("Секретный ключ live_… или test_…", text: $secretKey)
                        // oneTimeCode: система не предлагает сохранить ключ как пароль в связке ключей.
                        .textContentType(.oneTimeCode)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .privacySensitive()
                        .padding(12)
                }
                .font(InkFont.callout)
                .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Ink.fill))
                Text("Ключи — в личном кабинете ЮKassa: «Интеграция» → «Ключи API». Ключ хранится на сервере зашифрованным и в приложении не сохраняется.")
                    .font(InkFont.caption).foregroundStyle(Ink.secondary)
                Button {
                    connectPayments()
                } label: {
                    if connecting { ProgressView().tint(.white) } else { Text("Подключить") }
                }
                .buttonStyle(InkPrimaryButtonStyle())
                .disabled(connecting || shopId.isEmpty || secretKey.isEmpty)
                if !(store.userPlan?.proFeaturesOpen() ?? true) {
                    Text("ЮKassa — на тарифе Pro. Пригласите коллегу — вы оба получите месяц Pro.")
                        .font(InkFont.caption).foregroundStyle(Ink.amberInk)
                }
            } else {
                Text("Проверяем…").font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
            if !paymentsError.isEmpty { InkNotice(text: paymentsError) }
        }
        .inkCard()
        .confirmationDialog("Отключить ЮKassa? Новые записи с предоплатой будут оплачиваться по ссылке или реквизитам.",
                            isPresented: $confirmDisconnect, titleVisibility: .visible) {
            Button("Отключить", role: .destructive) { store.disconnectPayments() }
            Button("Оставить", role: .cancel) {}
        }
    }

    private func connectPayments() {
        // Секрет берём из поля и сразу очищаем: дальше он живёт только в аргументе запроса.
        let secret = secretKey
        let shop = shopId
        secretKey = ""
        if let problem = YooKassaCredentials.validate(shopId: shop, secretKey: secret) {
            paymentsError = problem
            return
        }
        paymentsError = ""
        connecting = true
        Task { @MainActor in
            do {
                try await store.connectPayments(shopId: shop, secretKey: secret)
                shopId = ""
            } catch {
                paymentsError = store.describe(error)
            }
            connecting = false
        }
    }

    // MARK: Календарь

    private var calendarCard: some View {
        let feed = store.calendarFeed
        return VStack(alignment: .leading, spacing: 12) {
            cardHeader("Календарь в телефоне", symbol: "calendar", tint: Ink.red,
                       status: feed.map { $0.enabled ? StatusView("Включена", .green, .check)
                                                     : StatusView("Выключена", .neutral, .minus) })
            Text("Ваши записи и принятые приглашения — в Календаре iOS, Google или Outlook по ссылке-подписке. Обновляется раз в час.")
                .font(InkFont.footnote).foregroundStyle(Ink.secondary)
            if let link = store.calendarFeedLink {
                InkNotice(text: "Ваша ссылка — покажем её только сейчас. Не публикуйте: по ней видны ваши встречи.", tone: .amber)
                Text(link).font(InkFont.footnote.monospaced()).foregroundStyle(Ink.label)
                    .lineLimit(2).truncationMode(.middle).textSelection(.enabled)
                if let webcal = CalendarFeed.webcal(link), let url = URL(string: webcal) {
                    Button("Добавить в Календарь") { openURL(url) }
                        .buttonStyle(InkPrimaryButtonStyle())
                }
                HStack(spacing: 8) {
                    ShareLink(item: link) {
                        Label("Поделиться", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                    Button {
                        UIPasteboard.general.string = link
                        store.setStatus("Ссылка скопирована")
                    } label: {
                        Label("Скопировать", systemImage: "doc.on.doc")
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                }
            } else if let feed, feed.enabled {
                Text("Ссылка показывается один раз. Потеряли её или хотите отключить старую — создайте новую.")
                    .font(InkFont.callout).foregroundStyle(Ink.label)
            }
            if let feed {
                HStack(spacing: 8) {
                    Button {
                        if feed.enabled { confirmReset = true } else { resetFeed() }
                    } label: {
                        if feedBusy { ProgressView() } else { Text(feed.enabled ? "Новая ссылка" : "Включить") }
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                    .disabled(feedBusy)
                    if feed.enabled {
                        Button("Выключить") { confirmDisableFeed = true }
                            .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
                    }
                }
            } else {
                Text("Проверяем…").font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
            if !feedError.isEmpty { InkNotice(text: feedError) }
        }
        .inkCard()
        .confirmationDialog("Старая ссылка перестанет работать во всех календарях, где она добавлена. Создать новую?",
                            isPresented: $confirmReset, titleVisibility: .visible) {
            Button("Да, новая ссылка", role: .destructive) { resetFeed() }
            Button("Отмена", role: .cancel) {}
        }
        .confirmationDialog("Календари перестанут получать ваши встречи. Выключить подписку?",
                            isPresented: $confirmDisableFeed, titleVisibility: .visible) {
            Button("Выключить", role: .destructive) { store.disableCalendarFeed() }
            Button("Отмена", role: .cancel) {}
        }
    }

    private func resetFeed() {
        feedBusy = true
        feedError = ""
        Task { @MainActor in
            do {
                try await store.resetCalendarFeed()
            } catch {
                feedError = store.describe(error)
            }
            feedBusy = false
        }
    }

    // MARK: Общее

    private func cardHeader(_ title: String, symbol: String, tint: Color, status: StatusView?) -> some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 34, height: 34)
                .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(tint))
            Text(title).font(InkFont.headline).foregroundStyle(Ink.label)
            Spacer(minLength: 4)
            if let status { StatusBadge(status: status, compact: true) }
        }
    }
}
