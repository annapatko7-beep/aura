// VoiceOverlay.swift — панель голосового запроса (SFSpeechRecognizer,
// fallback — серверный STT Aura).
import SwiftUI

struct VoiceOverlay: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        VStack(spacing: 12) {
            Text(store.speech.transcript.isEmpty ? "Слушаю…" : store.speech.transcript)
                .font(.body)
                .lineLimit(3)
                .padding(.horizontal)

            HStack(spacing: 24) {
                Button(role: .cancel) {
                    store.speech.cancelListening()
                    store.voiceOverlayVisible = false
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.largeTitle)
                }

                Button {
                    if store.speech.state == .listening {
                        store.speech.stopListening()
                    } else {
                        store.speech.startListening()
                    }
                } label: {
                    Image(systemName: store.speech.state == .listening ? "stop.circle.fill" : "mic.circle.fill")
                        .font(.system(size: 56))
                }
            }
        }
        .padding()
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 20))
        .padding()
        .onAppear {
            if store.speech.state == .idle {
                store.speech.startListening()
            }
        }
    }
}
