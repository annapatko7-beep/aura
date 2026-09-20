// AuraApp.swift — точка входа iOS-приложения Aura.
import AuraKit
import SwiftUI

@main
struct AuraApp: App {

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
                        Task { await store.askAura(text) }
                    }
                }
                .onOpenURL { url in
                    store.handle(url: url)
                }
        }
    }
}
