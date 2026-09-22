// SettingsView.swift — аккаунт, сервер, озвучка, разрешения, интеграции,
// Quick Actions (Back Tap → Команды).
import SwiftUI

struct SettingsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        NavigationStack {
            Form {
                Section("Аккаунт") {
                    LabeledContent("Имя", value: store.userName)
                    LabeledContent("Email", value: store.userEmail)
                    LabeledContent("Сервер", value: store.serverURLText)
                    LabeledContent("Соединение", value: store.connected ? "онлайн" : "офлайн")
                    Button("Выйти", role: .destructive) {
                        store.logout()
                    }
                }

                Section("Голос и озвучка") {
                    Toggle("Озвучивать ответы Ауры", isOn: $store.ttsEnabled)
                    Button("Остановить озвучку") {
                        store.speech.stopSpeaking()
                    }
                    Button("Проверить голосовой ввод") {
                        store.voiceOverlayVisible = true
                    }
                }

                Section("Quick Actions") {
                    NavigationLink {
                        QuickActionsView()
                    } label: {
                        Label("Back Tap и Команды", systemImage: "hand.tap")
                    }
                }

                Section("Безопасность") {
                    NavigationLink {
                        PermissionsView()
                    } label: {
                        Label("Разрешения инструментов", systemImage: "lock.shield")
                    }
                    NavigationLink {
                        ConfirmationsView()
                    } label: {
                        Label("Подтверждения", systemImage: "exclamationmark.shield")
                    }
                }

                Section("Интеграции") {
                    NavigationLink {
                        IntegrationsView()
                    } label: {
                        Label("Google Календарь и Gmail", systemImage: "puzzlepiece.extension")
                    }
                }
            }
            .navigationTitle("Настройки")
        }
    }
}

struct PermissionsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        List {
            ForEach(store.permissions.indices, id: \.self) { index in
                let tool = store.permissions[index]
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(tool.string("tool"))
                            .font(.headline)
                        if tool.bool("dangerous") {
                            Text("опасный")
                                .font(.caption2)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 2)
                                .background(Color.orange.opacity(0.2), in: Capsule())
                        }
                    }
                    Text(tool.string("description"))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Picker("Режим", selection: .init(
                        get: { tool.string("mode", default: "ask") },
                        set: { store.setPermission(tool: tool.string("tool"), mode: $0) })) {
                        Text("Разрешить").tag("allow")
                        Text("Спрашивать").tag("ask")
                        Text("Запретить").tag("deny")
                    }
                    .pickerStyle(.segmented)
                }
                .padding(.vertical, 2)
            }
        }
        .navigationTitle("Разрешения")
        .onAppear { store.loadPermissions() }
    }
}

struct IntegrationsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        List {
            Section("Провайдеры") {
                ForEach(store.integrationProviders.indices, id: \.self) { index in
                    let provider = store.integrationProviders[index]
                    let providerId = provider.string("provider")
                    let connection = store.integrations.first { $0.string("provider") == providerId }
                    VStack(alignment: .leading, spacing: 4) {
                        Text(provider.string("name"))
                            .font(.headline)
                        Text(provider.string("description"))
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                        if let connection {
                            HStack {
                                Text(connection.string("status") == "active" ? "подключено" : connection.string("status"))
                                    .font(.caption)
                                    .foregroundStyle(connection.string("status") == "active" ? .green : .orange)
                                if !connection.string("account").isEmpty {
                                    Text(connection.string("account"))
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                            }
                            HStack(spacing: 16) {
                                Button("Синхронизировать") {
                                    store.syncIntegration(id: connection.int("id"))
                                }
                                Button("Отключить", role: .destructive) {
                                    store.revokeIntegration(id: connection.int("id"))
                                }
                            }
                            .font(.footnote)
                        } else {
                            Button("Подключить") {
                                store.beginIntegration(provider: providerId)
                            }
                            .font(.footnote)
                        }
                    }
                    .padding(.vertical, 2)
                }
            }

            Section {
                Text("Подключение идёт через официальный OAuth Google в браузере; "
                     + "после разрешения вы вернётесь в приложение автоматически. "
                     + "Токены хранятся на сервере зашифрованными.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Интеграции")
        .onAppear { store.loadIntegrations() }
    }
}
