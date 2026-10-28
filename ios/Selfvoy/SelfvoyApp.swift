// SelfvoyApp.swift — точка входа iOS-приложения Selfvoy.
import SelfvoyKit
import SwiftUI
import UIKit
import UserNotifications

/// APNs: получаем device token и передаём в AppStore для регистрации
/// на сервере (devices.push.register, платформа "apns").
final class AppDelegate: NSObject, UIApplicationDelegate {
    static var onPushToken: ((String) -> Void)?
    static var onPushError: ((String) -> Void)?

    func application(_ application: UIApplication,
                     didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        // Токен — бинарные байты; APNs ожидает hex-строку без разделителей.
        let hex = deviceToken.map { String(format: "%02x", $0) }.joined()
        AppDelegate.onPushToken?(hex)
    }

    func application(_ application: UIApplication,
                     didFailToRegisterForRemoteNotificationsWithError error: Error) {
        // Не фатально: in-app уведомления (WS-события) продолжают работать.
        AppDelegate.onPushError?(error.localizedDescription)
    }
}

@main
struct SelfvoyApp: App {

    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var store = AppStore()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(store)
                .task {
                    store.bootstrap()
                    // Серверный STT как fallback распознавания на устройстве.
                    store.speech.serverTranscribe = { audio, completion in
                        Task {
                            do {
                                let payload = try await store.client.speechTranscribe(
                                    audioBase64: audio.base64EncodedString(),
                                    language: "ru",
                                    format: "wav")
                                completion(payload.string("text"))
                            } catch {
                                completion(nil)
                            }
                        }
                    }
                    store.speech.onFinalTranscript = { text in
                        store.voiceOverlayVisible = false
                        store.selectTab(.selfvoy)
                        store.ask(text)
                    }
                }
                .task(id: store.authState == .loggedIn) {
                    // Push (этап 13): разрешение → регистрация APNs. Спрашиваем после
                    // входа: системный диалог поверх экрана логина сбивает с толку,
                    // а токен до входа всё равно не к кому привязать.
                    guard store.authState == .loggedIn else { return }
                    AppDelegate.onPushToken = { token in
                        Task { @MainActor in store.registerPushToken(token) }
                    }
                    // Push недоступен (симулятор, сборка без подписи/entitlement) —
                    // это не ошибка пользователя: системный текст вроде «no valid
                    // aps-environment entitlement» в плашку не выводим, только в лог.
                    AppDelegate.onPushError = { message in
                        print("push: регистрация APNs не удалась: \(message)")
                    }
                    let center = UNUserNotificationCenter.current()
                    let granted = (try? await center.requestAuthorization(
                        options: [.alert, .sound, .badge])) ?? false
                    if granted {
                        await MainActor.run {
                            UIApplication.shared.registerForRemoteNotifications()
                        }
                    }
                }
                .onOpenURL { url in
                    store.handle(url: url)
                }
        }
    }
}
