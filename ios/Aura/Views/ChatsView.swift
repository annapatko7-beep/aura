// ChatsView.swift — список чатов и экран чата (сообщения + Аура).
import SwiftUI

struct ChatsView: View {

    @EnvironmentObject private var store: AppStore
    @State private var newContact = ""
    @State private var showNewChat = false

    var body: some View {
        NavigationStack {
            List {
                ForEach(store.chats.indices, id: \.self) { index in
                    let chat = store.chats[index]
                    Button {
                        store.selectChat(chat)
                    } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(chat.string("title", default: chat.string("peer_email")))
                                .foregroundStyle(.primary)
                            if let last = chat["last_message"]?.string("text") {
                                Text(last)
                                    .font(.footnote)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Чаты")
            .toolbar {
                Button {
                    showNewChat = true
                } label: {
                    Image(systemName: "square.and.pencil")
                }
            }
            .alert("Новый чат", isPresented: $showNewChat) {
                TextField("email собеседника", text: $newContact)
                Button("Открыть") {
                    store.openChat(contact: newContact)
                    newContact = ""
                }
                Button("Отмена", role: .cancel) {}
            }
            .sheet(isPresented: .init(
                get: { store.currentChatId != 0 },
                set: { if !$0 { store.currentChatId = 0 } })) {
                ChatView()
            }
            .refreshable { await store.loadAll() }
        }
    }
}

struct ChatView: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss

    @State private var draft = ""
    @State private var askMode = false

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 8) {
                            ForEach(store.messages.indices, id: \.self) { index in
                                let message = store.messages[index]
                                MessageBubble(message: message, own: message.int("sender_id") == 0
                                              || message.string("sender_email") == store.userEmail)
                                    .id(index)
                            }
                        }
                        .padding()
                    }
                    .onChange(of: store.messages.count) { _, count in
                        if count > 0 { proxy.scrollTo(count - 1, anchor: .bottom) }
                    }
                }

                VStack(spacing: 8) {
                    Toggle(isOn: $askMode) {
                        Label("Спросить Ауру", systemImage: "sparkles")
                            .font(.footnote)
                    }
                    .toggleStyle(.button)

                    HStack {
                        TextField(askMode ? "Сообщение для Ауры" : "Сообщение", text: $draft)
                            .textFieldStyle(.roundedBorder)
                        Button {
                            store.voiceOverlayVisible = true
                        } label: {
                            Image(systemName: "mic")
                        }
                        Button {
                            send()
                        } label: {
                            Image(systemName: "arrow.up.circle.fill")
                                .font(.title2)
                        }
                        .disabled(draft.isEmpty)
                    }
                }
                .padding()
            }
            .navigationTitle(store.currentChatTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                Button("Закрыть") {
                    store.currentChatId = 0
                    dismiss()
                }
            }
        }
    }

    private func send() {
        let text = draft
        draft = ""
        if askMode {
            Task { await store.askAura(text) }
        } else {
            store.sendMessage(text)
        }
    }
}

struct MessageBubble: View {

    let message: JSONValue
    let own: Bool

    var body: some View {
        HStack {
            if own { Spacer(minLength: 40) }
            VStack(alignment: own ? .trailing : .leading, spacing: 2) {
                Text(message.string("text"))
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(own ? Color.accentColor.opacity(0.25) : Color(.secondarySystemBackground),
                                in: RoundedRectangle(cornerRadius: 14))
                Text(message.string("created_at"))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            if !own { Spacer(minLength: 40) }
        }
    }
}
