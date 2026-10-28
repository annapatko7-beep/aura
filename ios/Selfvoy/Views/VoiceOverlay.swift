// VoiceOverlay.swift — голосовой запрос (дизайн 2026): расшифровка в реальном времени,
// по окончании текст уходит Selfvoy (SpeechService.onFinalTranscript).
import SwiftUI

struct VoiceOverlay: View {

    @EnvironmentObject private var store: AppStore
    @ObservedObject private var speech: SpeechService

    init(speech: SpeechService) {
        _speech = ObservedObject(wrappedValue: speech)
    }

    var body: some View {
        let listening = speech.state == .listening
        VStack(spacing: 16) {
            InkMark(size: 56)
                .scaleEffect(listening ? 1.08 : 1)
                .animation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true), value: speech.state)
            // Состояние — словом, не только анимацией.
            Text(listening ? "Слушаю" : "Голосовой запрос")
                .font(InkFont.eyebrow)
                .foregroundStyle(Ink.secondary)
                .textCase(.uppercase)
            Text(speech.transcript.isEmpty ? (listening ? "Говорите…" : "Нажмите на микрофон")
                 : speech.transcript)
                .font(InkFont.headline)
                .foregroundStyle(Ink.label)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
            HStack(spacing: 20) {
                Button {
                    speech.cancelListening()
                    store.voiceOverlayVisible = false
                } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(Ink.label)
                        .frame(width: 52, height: 52)
                        .background(Circle().fill(Ink.fill))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Закрыть")
                Button {
                    if listening {
                        speech.stopListening()
                    } else {
                        speech.startListening()
                    }
                } label: {
                    Image(systemName: listening ? "stop.fill" : "mic.fill")
                        .font(.system(size: 26, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: 68, height: 68)
                        .background(Circle().fill(Ink.accent))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(listening ? "Готово" : "Слушать")
            }
        }
        .padding(24)
        .background(RoundedRectangle(cornerRadius: 32, style: .continuous).fill(Ink.raised))
        .shadow(color: .black.opacity(0.18), radius: 24, y: 8)
        .padding(.horizontal, 16)
        .padding(.bottom, 90)
        .onAppear {
            if speech.state == .idle { speech.startListening() }
        }
    }
}
