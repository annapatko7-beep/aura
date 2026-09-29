// WebSocketTransport.swift — клиент WS поверх URLSessionWebSocketTask.
//
// Один сокет на сессию: запросы сопоставляются с ответами по id, события
// (type=event) рассылаются подписчикам через AsyncStream. TLS поддерживает
// сама URLSession (wss://) — в отличие от C++-сервера, которому TLS
// терминирует обратный прокси.
import Foundation

public final class WebSocketTransport: NSObject, @unchecked Sendable {

    public enum State: String, Sendable {
        case disconnected
        case connecting
        case connected
    }

    private let urlSession: URLSession
    private var task: URLSessionWebSocketTask?
    private var pending: [String: CheckedContinuation<JSONValue, Error>] = [:]
    private var eventStreams: [UUID: AsyncStream<AuraEvent>.Continuation] = [:]
    private let lock = NSLock()

    public private(set) var state: State = .disconnected
    public var onStateChange: ((State) -> Void)?

    public struct AuraEvent: Sendable {
        public let name: String
        public let payload: JSONValue
    }

    public override init() {
        self.urlSession = URLSession(configuration: .default)
        super.init()
    }

    // MARK: - Подключение

    public func connect(url: URL) throws {
        lock.lock()
        guard state == .disconnected else {
            lock.unlock()
            return
        }
        state = .connecting
        lock.unlock()
        onStateChange?(.connecting)

        let task = urlSession.webSocketTask(with: url)
        self.task = task
        task.resume()

        lock.lock()
        state = .connected
        lock.unlock()
        onStateChange?(.connected)
        receiveLoop(task)
    }

    public func disconnect() {
        lock.lock()
        let task = self.task
        self.task = nil
        let continuations = pending
        pending.removeAll()
        state = .disconnected
        lock.unlock()

        task?.cancel(with: .goingAway, reason: nil)
        for continuation in continuations.values {
            continuation.resume(throwing: AuraError(code: "not_connected",
                                                    message: "соединение закрыто"))
        }
        onStateChange?(.disconnected)
    }

    // MARK: - Запрос/ответ

    public func request(_ type: String, _ payload: JSONValue = .emptyObject) async throws -> JSONValue {
        guard let task else {
            throw AuraError(code: "not_connected", message: "нет соединения с сервером")
        }
        let id = UUID().uuidString
        let frame = AuraRequestFrame(id: id, type: type, payload: payload)
        let data = try JSONEncoder().encode(frame)
        guard let text = String(data: data, encoding: .utf8) else {
            throw AuraError(code: "internal", message: "не удалось собрать запрос")
        }

        return try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            pending[id] = continuation
            lock.unlock()

            task.send(.string(text)) { [weak self] error in
                guard let error else { return }
                guard let self else { return }
                self.lock.lock()
                let stored = self.pending.removeValue(forKey: id)
                self.lock.unlock()
                stored?.resume(throwing: AuraError(code: "transport",
                                                   message: error.localizedDescription))
            }
        }
    }

    // MARK: - События

    public func events() -> AsyncStream<AuraEvent> {
        let id = UUID()
        return AsyncStream { continuation in
            lock.lock()
            eventStreams[id] = continuation
            lock.unlock()
            continuation.onTermination = { [weak self] _ in
                self?.lock.lock()
                self?.eventStreams.removeValue(forKey: id)
                self?.lock.unlock()
            }
        }
    }

    // MARK: - Приём

    private func receiveLoop(_ task: URLSessionWebSocketTask) {
        task.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                // Разрыв соединения: будим ожидающие запросы, сообщаем о состоянии.
                self.lock.lock()
                let continuations = self.pending
                self.pending.removeAll()
                let wasConnected = self.state != .disconnected
                self.state = .disconnected
                self.task = nil
                self.lock.unlock()
                for continuation in continuations.values {
                    continuation.resume(throwing: AuraError(code: "transport",
                                                            message: error.localizedDescription))
                }
                if wasConnected { self.onStateChange?(.disconnected) }
            case .success(let message):
                switch message {
                case .string(let text):
                    self.handleFrame(text)
                case .data(let data):
                    if let text = String(data: data, encoding: .utf8) {
                        self.handleFrame(text)
                    }
                @unknown default:
                    break
                }
                self.receiveLoop(task)
            }
        }
    }

    private func handleFrame(_ text: String) {
        guard let data = text.data(using: .utf8),
              let frame = try? JSONDecoder().decode(AuraResponseFrame.self, from: data) else {
            return
        }
        if frame.type == "event", let name = frame.event {
            let event = AuraEvent(name: name, payload: frame.payload ?? .emptyObject)
            lock.lock()
            let continuations = Array(eventStreams.values)
            lock.unlock()
            for continuation in continuations {
                continuation.yield(event)
            }
            return
        }
        guard let id = frame.id else { return }
        lock.lock()
        let continuation = pending.removeValue(forKey: id)
        lock.unlock()
        guard let continuation else { return }
        if frame.type == "ok" {
            continuation.resume(returning: frame.payload ?? .emptyObject)
        } else {
            continuation.resume(throwing: AuraError(
                code: frame.code ?? "error",
                message: frame.message ?? "сервер вернул ошибку"))
        }
    }
}
