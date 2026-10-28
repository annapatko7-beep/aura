// AskSelfvoyIntent.swift — «Спросить Selfvoy» для Siri, Команд и Back Tap.
//
// Back Tap напрямую сторонним приложениям недоступен (системный жест) —
// поэтому честная схема: жест назначается в «Универсальный доступ → Касание
// → Касание сзади» на команду «Спросить Selfvoy», которая выполняет этот
// App Intent. Пошаговая инструкция — в приложении (QuickActionsView).
import AppIntents
import SelfvoyKit
import Foundation

struct AskSelfvoyIntent: AppIntent {

    static var title: LocalizedStringResource = "Спросить Selfvoy"
    static var description = IntentDescription("Отправить вопрос Selfvoy и получить ответ.")
    static var openAppWhenRun: Bool = false

    @Parameter(title: "Сообщение", requestValueDialog: "Что сказать Selfvoy?")
    var message: String

    func perform() async throws -> some IntentResult & ProvidesDialog & ReturnsValue<String> {
        let reply = try await IntentSession.shared.ask(message: message)
        return .result(value: reply, dialog: IntentDialog(stringLiteral: reply))
    }
}
