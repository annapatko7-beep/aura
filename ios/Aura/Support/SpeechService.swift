// SpeechService.swift — голос: распознавание и озвучка.
//
// Гибрид по проекту: on-device SFSpeechRecognizer (основной путь), серверный
// STT Aura (speech.transcribe → Whisper-совместимый) — fallback, когда
// распознавание на устройстве недоступно/не разрешили. Озвучка —
// AVSpeechSynthesizer (платформенный TTS).
import AuraKit
import AVFoundation
import Foundation
import Speech

@MainActor
final class SpeechService: NSObject, ObservableObject {

    enum State: String {
        case idle
        case listening
        case processing
    }

    @Published var state: State = .idle
    @Published var transcript = ""
    @Published var available = SFSpeechRecognizer.authorizationStatus() != .restricted

    private let recognizer = SFSpeechRecognizer(locale: Locale(identifier: "ru-RU"))
    private let audioEngine = AVAudioEngine()
    private var recognitionRequest: SFSpeechAudioBufferRecognitionRequest?
    private var recognitionTask: SFSpeechRecognitionTask?
    private var pcmBuffer = Data()          // накопленный PCM для серверного fallback
    private var sampleRate: Double = 16_000

    /// Завершение распознавания: текст для отправки.
    var onFinalTranscript: ((String) -> Void)?
    /// Серверный STT (fallback): внедряется AppStore, чтобы не тянуть клиент сюда.
    var serverTranscribe: ((Data, @escaping (String?) -> Void) -> Void)?

    // MARK: Распознавание

    func startListening() {
        guard state == .idle else { return }
        transcript = ""
        pcmBuffer = Data()

        SFSpeechRecognizer.requestAuthorization { [weak self] status in
            Task { @MainActor in
                guard let self else { return }
                guard status == .authorized else {
                    self.available = false
                    self.startServerCapture()   // без on-device — сразу серверный путь
                    return
                }
                self.available = true
                self.startOnDeviceCapture()
            }
        }
    }

    private func startOnDeviceCapture() {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.record, mode: .measurement, options: .duckOthers)
            try session.setActive(true, options: .notifyOthersOnDeactivation)

            let request = SFSpeechAudioBufferRecognitionRequest()
            request.shouldReportPartialResults = true
            recognitionRequest = request

            let inputNode = audioEngine.inputNode
            let format = inputNode.outputFormat(forBus: 0)
            sampleRate = format.sampleRate
            inputNode.installTap(onBus: 0, bufferSize: 1024, format: format) { [weak self] buffer, _ in
                request.append(buffer)
                self?.accumulatePCM(buffer)
            }
            audioEngine.prepare()
            try audioEngine.start()
            state = .listening

            recognitionTask = recognizer?.recognitionTask(with: request) { [weak self] result, error in
                Task { @MainActor in
                    guard let self else { return }
                    if let result {
                        self.transcript = result.bestTranscription.formattedString
                    }
                    if error != nil || (result?.isFinal ?? false) {
                        self.finishOnDevice()
                    }
                }
            }
        } catch {
            startServerCapture()
        }
    }

    /// Fallback: просто копим PCM и по «Стоп» отправляем на серверный STT.
    private func startServerCapture() {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.record, mode: .measurement, options: .duckOthers)
            try session.setActive(true, options: .notifyOthersOnDeactivation)
            let inputNode = audioEngine.inputNode
            let format = inputNode.outputFormat(forBus: 0)
            sampleRate = format.sampleRate
            inputNode.installTap(onBus: 0, bufferSize: 1024, format: format) { [weak self] buffer, _ in
                self?.accumulatePCM(buffer)
            }
            audioEngine.prepare()
            try audioEngine.start()
            state = .listening
        } catch {
            state = .idle
        }
    }

    func stopListening() {
        guard state == .listening else { return }
        if recognitionRequest != nil {
            recognitionRequest?.endAudio()
        } else {
            finishWithServerSTT()
        }
    }

    func cancelListening() {
        recognitionTask?.cancel()
        recognitionTask = nil
        recognitionRequest = nil
        stopEngine()
        state = .idle
        transcript = ""
    }

    private func finishOnDevice() {
        stopEngine()
        recognitionRequest = nil
        recognitionTask = nil
        state = .idle
        if !transcript.isEmpty {
            onFinalTranscript?(transcript)
        }
    }

    private func finishWithServerSTT() {
        stopEngine()
        state = .processing
        let wav = Self.wavWrap(pcm: pcmBuffer, sampleRate: sampleRate)
        serverTranscribe?(wav) { [weak self] text in
            Task { @MainActor in
                guard let self else { return }
                self.state = .idle
                if let text, !text.isEmpty {
                    self.transcript = text
                    self.onFinalTranscript?(text)
                }
            }
        }
    }

    private func stopEngine() {
        audioEngine.inputNode.removeTap(onBus: 0)
        audioEngine.stop()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    // MARK: PCM → WAV (16-bit mono)

    private func accumulatePCM(_ buffer: AVAudioPCMBuffer) {
        guard let channel = buffer.floatChannelData?[0] else { return }
        let frames = Int(buffer.frameLength)
        var pcm = Data(capacity: frames * 2)
        for index in 0..<frames {
            let sample = Int16(clamping: Int(channel[index] * 32_767))
            var little = sample.littleEndian
            withUnsafeBytes(of: &little) { pcm.append(contentsOf: $0) }
        }
        pcmBuffer.append(pcm)
    }

    static func wavWrap(pcm: Data, sampleRate: Double) -> Data {
        var data = Data()
        func appendString(_ text: String) { data.append(Data(text.utf8)) }
        func appendUInt32(_ value: UInt32) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        func appendUInt16(_ value: UInt16) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        let rate = UInt32(sampleRate)
        appendString("RIFF")
        appendUInt32(UInt32(36 + pcm.count))
        appendString("WAVEfmt ")
        appendUInt32(16)
        appendUInt16(1)              // PCM
        appendUInt16(1)              // mono
        appendUInt32(rate)
        appendUInt32(rate * 2)       // byte rate
        appendUInt16(2)              // block align
        appendUInt16(16)             // bits per sample
        appendString("data")
        appendUInt32(UInt32(pcm.count))
        data.append(pcm)
        return data
    }

    // MARK: Озвучка (TTS)

    private let synthesizer = AVSpeechSynthesizer()

    func speak(_ text: String) {
        guard !text.isEmpty else { return }
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(language: "ru-RU")
        synthesizer.speak(utterance)
    }

    func stopSpeaking() {
        synthesizer.stopSpeaking(at: .immediate)
    }
}
