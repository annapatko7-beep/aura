// RootView.swift — каркас приложения: вкладки после входа, логин до входа.
import SwiftUI

struct RootView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        Group {
            if store.authState == .loggedIn {
                TabView(selection: $store.tab) {
                    ChatsView()
                        .tabItem { Label("Чаты", systemImage: "bubble.left.and.bubble.right") }
                        .tag(AppTab.chats)
                    TasksView()
                        .tabItem { Label("Задачи", systemImage: "checklist") }
                        .tag(AppTab.tasks)
                    ConfirmationsView()
                        .tabItem { Label("Подтверждения", systemImage: "exclamationmark.shield") }
                        .tag(AppTab.confirmations)
                        .badge(store.confirmations.count)
                    NotificationsView()
                        .tabItem { Label("Уведомления", systemImage: "bell") }
                        .tag(AppTab.notifications)
                        .badge(store.unreadNotifications > 0 ? Int(store.unreadNotifications) : 0)
                    SettingsView()
                        .tabItem { Label("Настройки", systemImage: "gearshape") }
                        .tag(AppTab.settings)
                }
                .overlay(alignment: .bottom) {
                    if store.voiceOverlayVisible {
                        VoiceOverlay()
                    }
                }
                .alert("Ошибка", isPresented: .init(
                    get: { !store.errorMessage.isEmpty },
                    set: { if !$0 { store.errorMessage = "" } })) {
                    Button("OK", role: .cancel) {}
                } message: {
                    Text(store.errorMessage)
                }
            } else {
                LoginView()
            }
        }
    }
}
