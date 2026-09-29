// NotificationsView.swift — «входящая» уведомлений и push-устройства (этап 13).
import SwiftUI

struct NotificationsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if store.notifications.isEmpty {
                        Text("Уведомлений пока нет")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(store.notifications.indices, id: \.self) { index in
                        let item = store.notifications[index]
                        Button {
                            if !item.bool("read") {
                                store.markNotificationsRead(id: item.int("id"))
                            }
                        } label: {
                            HStack(alignment: .top, spacing: 10) {
                                Image(systemName: Self.icon(for: item.string("kind")))
                                    .foregroundStyle(item.bool("read") ? .secondary : .accentColor)
                                    .frame(width: 24)
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(item.string("title"))
                                        .font(.subheadline.weight(item.bool("read") ? .regular : .semibold))
                                        .foregroundStyle(.primary)
                                    if !item.string("body").isEmpty {
                                        Text(item.string("body"))
                                            .font(.footnote)
                                            .foregroundStyle(.secondary)
                                    }
                                    Text(item.string("created_at"))
                                        .font(.caption2)
                                        .foregroundStyle(.tertiary)
                                }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                } header: {
                    Text("Уведомления")
                }

                Section {
                    if store.pushDevices.isEmpty {
                        Text("Push-устройства не зарегистрированы")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(store.pushDevices.indices, id: \.self) { index in
                        let device = store.pushDevices[index]
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(device.string("platform"))
                                    .font(.subheadline)
                                Text("зарегистрировано: \(device.string("created_at"))")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Отозвать", role: .destructive) {
                                store.revokePushDevice(id: device.int("id"))
                            }
                            .font(.footnote)
                        }
                    }
                } header: {
                    Text("Push-устройства")
                } footer: {
                    Text("Токен этого устройства регистрируется автоматически после разрешения на уведомления.")
                }
            }
            .navigationTitle("Уведомления")
            .toolbar {
                if store.unreadNotifications > 0 {
                    Button("Прочитать все") {
                        store.markNotificationsRead()
                    }
                }
            }
            .refreshable {
                store.loadNotifications()
                store.loadPushDevices()
            }
        }
    }

    static func icon(for kind: String) -> String {
        switch kind {
        case "task.due": return "alarm"
        case "confirmation.requested": return "exclamationmark.shield"
        case "a2a.proposal": return "person.2"
        case "login.new": return "arrow.right.square"
        case "twofactor.enabled", "twofactor.disabled": return "lock.shield"
        default: return "bell"
        }
    }
}
