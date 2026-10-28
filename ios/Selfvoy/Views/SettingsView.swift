// SettingsView.swift — настройки (дизайн 2026): профиль (имя и «о себе»,
// profile.update), что разрешено Selfvoy (allow / ask / deny по инструментам),
// приватность в переговорах (prefs.privacy), голос, безопасность (2FA),
// интеграции Google, Back Tap, адрес сервера и выход. Кнопки делают только то,
// что умеет сервер.
import SelfvoyKit
import SwiftUI
import UIKit

struct SettingsView: View {

    @EnvironmentObject private var store: AppStore
    @State private var serverURL = ""
    @State private var totpCode = ""
    @State private var disablePassword = ""
    @State private var showQuickActions = false
    @State private var editingProfile = false
    @State private var nameDraft = ""
    @State private var bioDraft = ""

    var body: some View {
        VStack(spacing: 0) {
            SubscreenBar()
            content
        }
        .inkScreen()
        .onAppear {
            serverURL = store.serverURLText
            store.loadPrivacy()
            if store.referral == nil { store.loadReferral() }
        }
        .sheet(isPresented: $showQuickActions) {
            NavigationStack { QuickActionsView() }
        }
    }

    private var content: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: store.connected ? "Соединение активно" : "Нет соединения", title: "Настройки")
                profile
                InkGroup {
                    InkCell(symbol: "calendar.badge.plus", tint: Ink.accent, title: "Страницы записи") {
                        store.selectTab(.booking)
                    }
                    InkDivider()
                    InkCell(symbol: "link", tint: Ink.teal, title: "Telegram, ЮKassa и календарь") {
                        store.selectTab(.integrations)
                    }
                }
                InkGroupHeader(title: "Пригласить друзей")
                referralSection
                permissionsSection
                InkGroupHeader(title: "Приватность ИИ")
                privacy
                InkGroupHeader(title: "Голос")
                voice
                InkGroupHeader(title: "Безопасность")
                security
                InkGroupHeader(title: "Интеграции")
                integrations
                InkGroupHeader(title: "Сервер")
                VStack(alignment: .leading, spacing: 10) {
                    InkField(text: $serverURL, placeholder: "ws:// или wss://", inCard: true, keyboard: .URL)
                    InkButton(title: "Сохранить и переподключиться", kind: .secondary) { store.setServerURL(serverURL) }
                    StatusBadge(status: store.connected ? StatusView("Соединение активно", .green, .check)
                                                        : StatusView("Нет соединения", .red, .cross),
                                compact: true)
                }
                .inkCard()
                InkButton(title: "Выйти", systemImage: "rectangle.portrait.and.arrow.right", kind: .destructive) {
                    store.logout()
                }
                .padding(.top, 8)
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .scrollDismissesKeyboard(.interactively)
    }

    /// Реферальный код и ссылка: друг заводит Selfvoy — засчитывается после подтверждения email.
    @ViewBuilder
    private var referralSection: some View {
        if let info = store.referral {
            VStack(alignment: .leading, spacing: 10) {
                InkFootnote("Друг заведёт свой Selfvoy — и ваши Selfvoy смогут договариваться о встречах напрямую. Регистрации со страниц записи тоже засчитываются вам.")
                Text(info.rewardText).font(InkFont.footnote.weight(.semibold)).foregroundStyle(Ink.amberInk)
                HStack {
                    Text("Ваш код").font(InkFont.callout).foregroundStyle(Ink.secondary)
                    Spacer()
                    Text(info.code).font(InkFont.headline.monospaced()).foregroundStyle(Ink.label).textSelection(.enabled)
                }
                if info.hasLink {
                    Text(info.link).font(InkFont.footnote).foregroundStyle(Ink.accent).textSelection(.enabled)
                } else {
                    InkFootnote("Сервер не знает свой публичный адрес (SELFVOY_PUBLIC_URL) — делимся только кодом: друг введёт его при регистрации.")
                }
                HStack(spacing: 8) {
                    Button {
                        UIPasteboard.general.string = info.link
                        store.setStatus(info.hasLink ? "Ссылка скопирована" : "Код скопирован")
                    } label: {
                        Label("Копировать", systemImage: "doc.on.doc").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                    ShareLink(item: info.shareText) {
                        Label("Поделиться", systemImage: "square.and.arrow.up").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                }
                Text("Зарегистрировались: \(info.joined) · подтвердили email: \(info.verified)")
                    .font(InkFont.caption)
                    .foregroundStyle(Ink.secondary)
                ForEach(info.referrals, id: \.self) { item in
                    HStack {
                        Text(item.displayName.isEmpty ? "Без имени" : item.displayName)
                            .font(InkFont.callout).foregroundStyle(Ink.label)
                        Spacer()
                        StatusBadge(status: item.verified ? StatusView("С Selfvoy", .green, .check)
                                                          : StatusView("Ждёт подтверждения", .neutral, .clock),
                                    compact: true)
                    }
                }
            }
            .inkCard()
        } else {
            InkEmpty(text: "Загружаем реферальный код…")
        }
    }

    @ViewBuilder
    private var permissionsSection: some View {
        InkGroupHeader(title: "Что может ваш Selfvoy")
        InkFootnote("Selfvoy только формирует намерение, исполняет сервер. Опасные действия по умолчанию ждут вашего подтверждения.")
            .padding(.horizontal, 16)
        if !store.permissions.isEmpty {
            InkGroup {
                ForEach(Array(store.permissions.enumerated()), id: \.element.id) { index, row in
                    if index > 0 { InkDivider(inset: 16) }
                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 8) {
                            Text(Format.toolLabel(row.tool)).font(InkFont.headline).foregroundStyle(Ink.label)
                            if row.dangerous {
                                StatusBadge(status: StatusView("Опасное", .attention, .hand), compact: true)
                            }
                        }
                        Text(Format.toolDescription(row.tool, fallback: row.description))
                            .font(InkFont.footnote)
                            .foregroundStyle(Ink.secondary)
                        ModePicker(mode: row.mode) { store.setPermission(tool: row.tool, mode: $0) }
                    }
                    .inkRow(vertical: 12)
                }
            }
        }
    }

    private var profile: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                InkAvatar(name: store.userName, size: 52)
                VStack(alignment: .leading, spacing: 3) {
                    Text(store.userName).font(InkFont.headline).foregroundStyle(Ink.label)
                    Text(store.userEmail).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                    if let plan = store.userPlan {
                        HStack(spacing: 6) {
                            PlanBadge(plan: plan)
                            Text(plan.hint()).font(InkFont.caption).foregroundStyle(Ink.secondary)
                        }
                        .padding(.top, 2)
                    }
                }
                Spacer(minLength: 0)
                if !editingProfile {
                    Button("Изменить") { startEditing() }
                        .buttonStyle(InkSecondaryButtonStyle())
                        .accessibilityLabel("Изменить профиль")
                }
            }
            if editingProfile {
                profileEditor
            } else if !store.userBio.isEmpty {
                Text(store.userBio).font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
        }
        .inkCard()
    }

    /// Имя 1–80 символов, «о себе» до 300; ошибка сервера — под полями.
    private var profileEditor: some View {
        let nameCount = nameDraft.trimmingCharacters(in: .whitespacesAndNewlines).count
        let bioCount = bioDraft.trimmingCharacters(in: .whitespacesAndNewlines).count
        let problem = SelfvoyProfileRules.validate(displayName: nameDraft, bio: bioDraft)
        let message = problem ?? store.profileError
        return VStack(alignment: .leading, spacing: 8) {
            InkField(text: $nameDraft, placeholder: "Как к вам обращаться", label: "Имя", inCard: true)
            Text("\(nameCount)/\(SelfvoyProfileRules.displayNameMax)").font(InkFont.caption).foregroundStyle(Ink.secondary)
            InkField(text: $bioDraft, placeholder: "Пара слов о себе", label: "О себе", inCard: true)
            Text("\(bioCount)/\(SelfvoyProfileRules.bioMax)").font(InkFont.caption).foregroundStyle(Ink.secondary)
            if !message.isEmpty {
                InkNotice(text: message)
            }
            HStack(spacing: 10) {
                InkButton(title: "Отмена", kind: .quiet) {
                    editingProfile = false
                    store.profileError = ""
                }
                InkButton(title: "Сохранить", busy: store.savingProfile, enabled: problem == nil) {
                    store.updateProfile(displayName: nameDraft, bio: bioDraft) { editingProfile = false }
                }
            }
        }
    }

    private func startEditing() {
        nameDraft = store.userName
        bioDraft = store.userBio
        store.profileError = ""
        editingProfile = true
    }

    /// Три флага prefs.privacy; переключение сохраняется сразу, при ошибке откатывается.
    private var privacy: some View {
        VStack(alignment: .leading, spacing: 8) {
            InkFootnote("Что ваш Selfvoy раскрывает чужому Selfvoy, когда договаривается о встрече.")
                .padding(.horizontal, 16)
            InkGroup {
                privacyToggle(.shareFreeSlotsOnly, title: "Делиться только свободными слотами",
                              hint: "Другому Selfvoy уходят только занятость и настройки, нужные для времени и места. Факты из памяти, день рождения и аллергии не передаются.")
                InkDivider(inset: 16)
                privacyToggle(.allowPlaceChoice, title: "Разрешить выбор локаций",
                              hint: "Selfvoy предлагает место встречи. Если выключено у вас или у собеседника, место не предлагается — выберете сами.")
                InkDivider(inset: 16)
                privacyToggle(.hideEventTitles, title: "Скрывать названия других событий",
                              hint: "Названия событий вашего календаря заменяются на «занято».")
            }
        }
    }

    private func privacyToggle(_ flag: SelfvoyPrivacy.Flag, title: String, hint: String) -> some View {
        Toggle(isOn: Binding(get: { store.privacy[flag] }, set: { store.setPrivacy(flag, $0) })) {
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(InkFont.body).foregroundStyle(Ink.label)
                Text(hint).font(InkFont.footnote).foregroundStyle(Ink.secondary)
            }
        }
        .tint(Ink.accent)
        .inkRow()
    }

    private var voice: some View {
        InkGroup {
            Toggle(isOn: $store.ttsEnabled) {
                Text("Озвучивать ответы Selfvoy").font(InkFont.body).foregroundStyle(Ink.label)
            }
            .tint(Ink.accent)
            .inkRow()
            InkDivider(inset: 16)
            InkCell(symbol: "mic.fill", tint: Ink.accent, title: "Проверить голос") { store.voiceOverlayVisible = true }
            InkDivider()
            InkCell(symbol: "speaker.slash.fill", tint: Color(hex: 0x8E8E93), title: "Тишина") { store.speech.stopSpeaking() }
            InkDivider()
            InkCell(symbol: "hand.tap.fill", tint: Ink.violet, title: "Back Tap и Команды") { showQuickActions = true }
        }
    }

    private var security: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Двухфакторная аутентификация").font(InkFont.headline).foregroundStyle(Ink.label)
                Spacer(minLength: 8)
                StatusBadge(status: store.twoFaEnabled ? StatusView("Включена", .green, .check)
                                                       : StatusView("Выключена", .neutral, .minus),
                            compact: true)
            }
            Text("Код из приложения-аутентификатора (TOTP) при входе на новом устройстве.")
                .font(InkFont.footnote)
                .foregroundStyle(Ink.secondary)
            if !store.twoFaEnabled && store.twoFaSecret.isEmpty {
                InkButton(title: "Включить 2FA", kind: .secondary) { store.setup2fa() }
            }
            if !store.twoFaSecret.isEmpty {
                Text("Добавьте секрет в приложение-аутентификатор:").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                Text(store.twoFaSecret)
                    .font(InkFont.callout.monospaced())
                    .foregroundStyle(Ink.accent)
                    .textSelection(.enabled)
                InkField(text: $totpCode, placeholder: "Код из приложения", inCard: true, keyboard: .numberPad)
                InkButton(title: "Подтвердить и включить") {
                    store.confirm2fa(code: totpCode)
                    totpCode = ""
                }
            }
            if !store.recoveryCodes.isEmpty {
                InkNotice(text: "Резервные коды — сохраните, они показываются один раз:", tone: .attention)
                ForEach(store.recoveryCodes, id: \.self) { code in
                    Text(code).font(InkFont.callout.monospaced()).foregroundStyle(Ink.label).textSelection(.enabled)
                }
            }
            if store.twoFaEnabled {
                InkField(text: $disablePassword, placeholder: "Пароль", secure: true, inCard: true)
                InkButton(title: "Отключить 2FA", kind: .destructive) {
                    store.disable2fa(password: disablePassword)
                    disablePassword = ""
                }
            }
        }
        .inkCard()
    }

    private var integrations: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Google Календарь и Gmail через официальный OAuth. Токены хранятся только на сервере, зашифрованными.")
                .font(InkFont.footnote)
                .foregroundStyle(Ink.secondary)
            if store.providers.isEmpty {
                Text("Сервер не предлагает интеграций.").font(InkFont.callout).foregroundStyle(Ink.secondary)
            }
            ForEach(store.providers) { provider in
                let connection = store.connections.first { $0.provider == provider.provider }
                VStack(alignment: .leading, spacing: 6) {
                    HStack {
                        Text(provider.name).font(InkFont.headline).foregroundStyle(Ink.label)
                        Spacer(minLength: 8)
                        if let connection {
                            StatusBadge(status: connection.status == "active"
                                        ? StatusView("Подключено", .green, .check)
                                        : StatusView(connection.status, .attention, .clock),
                                        compact: true)
                        }
                    }
                    if !provider.description.isEmpty {
                        Text(provider.description).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                    }
                    if let connection {
                        if connection.status == "active" && !connection.account.isEmpty {
                            Text(connection.account).font(InkFont.callout).foregroundStyle(Ink.label)
                        }
                        HStack(spacing: 10) {
                            InkButton(title: "Синхронизировать", kind: .secondary) { store.syncIntegration(id: connection.id) }
                            InkButton(title: "Отключить", kind: .destructive) {
                                store.revokeIntegration(id: connection.id)
                            }
                        }
                    } else {
                        InkButton(title: "Подключить", systemImage: "link", kind: .secondary) {
                            store.beginIntegration(provider: provider.provider)
                        }
                    }
                }
            }
        }
        .inkCard()
    }
}

/// Три режима инструмента: сам / спрашивать / запретить.
private struct ModePicker: View {
    let mode: String
    let onSelect: (String) -> Void

    var body: some View {
        Picker("Режим", selection: Binding(get: { mode }, set: { value in
            if value != mode { onSelect(value) }
        })) {
            Text("Сам").tag("allow")
            Text("Спрашивать").tag("ask")
            Text("Запретить").tag("deny")
        }
        .pickerStyle(.segmented)
        .labelsHidden()
    }
}
