// ContactsView.swift — контакты (дизайн 2026): люди из чатов, поиск новых
// (users.search → chat.open), карточка человека с «Начать переговоры» /
// «Открыть чат» и сама переписка.
import SelfvoyKit
import SwiftUI

struct ContactsView: View {

    @EnvironmentObject private var store: AppStore
    @State private var query = ""
    @State private var recentOnly = false
    @State private var selected: Contact?
    @FocusState private var searchFocused: Bool

    var body: some View {
        Group {
            if store.openChatId != 0 {
                ChatView(chatId: store.openChatId,
                         title: store.contacts.first { $0.chatId == store.openChatId }?.name
                            ?? store.chats.first { $0.id == store.openChatId }?.title ?? "")
            } else if let contact = selected {
                ContactCard(contact: contact) { selected = nil }
            } else {
                list
            }
        }
        .inkScreen()
    }

    private var list: some View {
        let visible = store.contacts.filter { contact in
            (!recentOnly || !contact.lastAt.isEmpty)
                && (query.isEmpty || contact.name.localizedCaseInsensitiveContains(query)
                    || contact.email.localizedCaseInsensitiveContains(query))
        }
        let newPeople = store.foundUsers.filter { found in !store.contacts.contains { $0.userId == found.id } }

        return ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Люди и их Selfvoy", title: "Контакты")
                searchField
                Picker("Показать", selection: $recentOnly) {
                    Text("Все").tag(false)
                    Text("Недавние").tag(true)
                }
                .pickerStyle(.segmented)
                if visible.isEmpty && newPeople.isEmpty {
                    InkEmpty(text: query.isEmpty ? "Пока нет собеседников. Найдите человека по имени или email."
                                                 : "Никого не нашли")
                }
                if !visible.isEmpty {
                    InkGroup {
                        ForEach(Array(visible.enumerated()), id: \.element.id) { index, contact in
                            if index > 0 { InkDivider(inset: 68) }
                            Button {
                                selected = contact
                            } label: {
                                HStack(spacing: 12) {
                                    InkAvatar(name: contact.name, size: 42)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(contact.name).font(InkFont.headline).foregroundStyle(Ink.label)
                                            .lineLimit(1)
                                        Text(contact.lastBody.isEmpty ? "Selfvoy доступен" : contact.lastBody)
                                            .font(InkFont.footnote)
                                            .foregroundStyle(Ink.secondary)
                                            .lineLimit(1)
                                    }
                                    Spacer(minLength: 8)
                                    if !contact.lastAt.isEmpty {
                                        Text(Format.dayPhrase(contact.lastAt))
                                            .font(InkFont.footnote)
                                            .foregroundStyle(Ink.secondary)
                                    }
                                    Image(systemName: "chevron.right")
                                        .font(.system(size: 13, weight: .semibold))
                                        .foregroundStyle(Ink.tertiary)
                                }
                                .inkRow(vertical: 10)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
                if !newPeople.isEmpty {
                    InkGroupHeader(title: "Найдено в Selfvoy")
                    InkGroup {
                        ForEach(Array(newPeople.enumerated()), id: \.element.id) { index, user in
                            if index > 0 { InkDivider(inset: 66) }
                            HStack(spacing: 12) {
                                InkAvatar(name: user.name, size: 38)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(user.name).font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                                    Text(user.online ? "в сети" : "есть Selfvoy")
                                        .font(InkFont.footnote)
                                        .foregroundStyle(Ink.secondary)
                                }
                                Spacer(minLength: 8)
                                Button {
                                    // chat.open принимает id пользователя строкой.
                                    store.openChat(contact: String(user.id))
                                    query = ""
                                } label: {
                                    Label("Добавить", systemImage: "person.badge.plus")
                                }
                                .buttonStyle(InkSecondaryButtonStyle())
                            }
                            .inkRow(vertical: 10)
                        }
                    }
                }
                InkButton(title: "Пригласить в Selfvoy", systemImage: "person.badge.plus", kind: .secondary,
                          fullWidth: false) {
                    searchFocused = true
                }
                .padding(.top, 6)
                InkFootnote("Найдите человека по email: если у него есть Selfvoy, он появится в списке.")
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .scrollDismissesKeyboard(.interactively)
        .task(id: query) {
            try? await Task.sleep(nanoseconds: 300_000_000)
            if !Task.isCancelled { store.searchUsers(query) }
        }
    }

    /// Поиск как в макете: мягкая заливка, лупа, без рамки.
    private var searchField: some View {
        HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Ink.secondary)
                .accessibilityHidden(true)
            TextField("Искать людей или Selfvoy", text: $query)
                .font(InkFont.body)
                .foregroundStyle(Ink.label)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .focused($searchFocused)
            if !query.isEmpty {
                Button {
                    query = ""
                } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(Ink.tertiary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Очистить поиск")
            }
        }
        .padding(.horizontal, 10)
        .frame(height: 38)
        .background(RoundedRectangle(cornerRadius: 11, style: .continuous).fill(Ink.fill))
    }
}

/// Карточка человека: кто он, что последнее в переписке и что может ваш Selfvoy.
private struct ContactCard: View {

    @EnvironmentObject private var store: AppStore
    let contact: Contact
    let onBack: () -> Void

    private func mode(_ tool: String) -> String {
        switch store.permissions.first(where: { $0.tool == tool })?.mode {
        case "allow": return "сам"
        case "ask": return "с вашего подтверждения"
        case "deny": return "запрещено"
        default: return "—"
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    InkRoundButton(systemImage: "chevron.left", label: "Назад", action: onBack)
                        .accessibilityIdentifier("nav.back")
                    Spacer()
                }
                VStack(spacing: 6) {
                    InkAvatar(name: contact.name, size: 88)
                    Text(contact.name)
                        .font(InkFont.title)
                        .foregroundStyle(Ink.label)
                        .multilineTextAlignment(.center)
                    HStack(spacing: 4) {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(Ink.green)
                        Text("Selfvoy доступен")
                    }
                    .font(InkFont.footnote.weight(.semibold))
                    .foregroundStyle(Ink.secondary)
                    Text(contact.email).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
                .frame(maxWidth: .infinity)
                InkButton(title: "Начать переговоры", systemImage: "person.2.fill", enabled: !store.busy) {
                    store.negotiate(target: contact.email, topic: "", durationMinutes: 60,
                                    windowHours: Format.windowHours("weekend"))
                }
                InkButton(title: "Открыть чат", systemImage: "bubble.left.fill", kind: .secondary) {
                    store.openChat(id: contact.chatId)
                }
                InkGroupHeader(title: "Последнее в переписке")
                InkGroup {
                    if contact.lastBody.isEmpty {
                        Text("Пока ничего").font(InkFont.callout).foregroundStyle(Ink.secondary).inkRow()
                    } else {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("«\(contact.lastBody)»").font(InkFont.body).foregroundStyle(Ink.label)
                            Text("\(Format.dayPhrase(contact.lastAt)), \(Format.timeOf(contact.lastAt))")
                                .font(InkFont.footnote)
                                .foregroundStyle(Ink.secondary)
                        }
                        .inkRow()
                    }
                }
                InkGroupHeader(title: "Что может ваш Selfvoy")
                InkGroup {
                    permissionRow("Писать", value: mode("send_message"), symbol: "paperplane.fill", tint: Ink.accent)
                    InkDivider()
                    permissionRow("Бронировать", value: mode("book_table"), symbol: "fork.knife", tint: Ink.amber)
                    InkDivider()
                    permissionRow("Делиться", value: "только свободными окнами", symbol: "lock.fill", tint: Ink.green)
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
    }

    private func permissionRow(_ title: String, value: String, symbol: String, tint: Color) -> some View {
        HStack(spacing: 12) {
            InkIconTile(symbol: symbol, tint: tint, size: 30)
            Text(title).font(InkFont.body).foregroundStyle(Ink.label)
            Spacer(minLength: 8)
            Text(value)
                .font(InkFont.callout)
                .foregroundStyle(Ink.secondary)
                .multilineTextAlignment(.trailing)
        }
        .inkRow()
    }
}

/// Переписка с человеком; режим «Спросить Selfvoy» отправляет agent.ask в этот чат.
private struct ChatView: View {

    @EnvironmentObject private var store: AppStore
    let chatId: Int64
    let title: String
    @State private var draft = ""
    @State private var toSelfvoy = false

    init(chatId: Int64, title: String) {
        self.chatId = chatId
        self.title = title
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                InkRoundButton(systemImage: "chevron.left", label: "Назад") { store.closeChat() }
                    .accessibilityIdentifier("nav.back")
                InkAvatar(name: title, size: 36)
                Text(title).font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
                Spacer()
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            Rectangle().fill(Ink.separator).frame(height: 0.5)
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 10) {
                        if store.messages.isEmpty {
                            InkEmpty(text: "Сообщений пока нет. Напишите или попросите Selfvoy договориться о встрече.")
                        }
                        ForEach(store.messages) { message in
                            let mine = message.senderId == store.userId
                            let agent = message.kind.hasPrefix("agent")
                            VStack(alignment: mine ? .trailing : .leading, spacing: 4) {
                                if agent {
                                    HStack(spacing: 4) {
                                        InkMark(size: 14)
                                        Text("Selfvoy").font(InkFont.caption).foregroundStyle(Ink.accent)
                                    }
                                }
                                Bubble(mine: mine, text: message.body)
                                Text("\(message.senderName.isEmpty ? (mine ? "Вы" : title) : message.senderName) · \(Format.timeOf(message.createdAt))")
                                    .font(InkFont.caption)
                                    .foregroundStyle(Ink.secondary)
                            }
                            .frame(maxWidth: .infinity, alignment: mine ? .trailing : .leading)
                            .id(message.id)
                        }
                    }
                    .padding(16)
                }
                .scrollDismissesKeyboard(.interactively)
                .onChange(of: store.messages.count) {
                    if let last = store.messages.last {
                        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
            Picker("Кому", selection: $toSelfvoy) {
                Text("Собеседнику").tag(false)
                Text("Спросить Selfvoy").tag(true)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 16)
            .padding(.top, 6)
            ChatComposer(draft: $draft,
                         placeholder: toSelfvoy ? "Например: договорись о встрече" : "Сообщение…",
                         sendSymbol: toSelfvoy ? "sparkles" : nil,
                         canSend: !draft.trimmingCharacters(in: .whitespaces).isEmpty && !store.busy,
                         onSend: send)
        }
    }

    private func send() {
        guard !draft.trimmingCharacters(in: .whitespaces).isEmpty, !store.busy else { return }
        if toSelfvoy {
            store.ask(draft, chatId: chatId)
        } else {
            store.sendMessage(draft)
        }
        draft = ""
    }
}
