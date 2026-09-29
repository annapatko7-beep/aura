// AskAuraIntent.swift — «Спросить Ауру» для Siri, Команд и Back Tap.
//
// Back Tap напрямую сторонним приложениям недоступен (системный жест) —
// поэтому честная схема: жест назначается в «Универсальный доступ → Касание
// → Касание сзади» на команду «Спросить Ауру», которая выполняет этот
// App Intent. Пошаговая инструкция — в приложении (QuickActionsView).
import AppIntents
import AuraKit
import Foundation

struct AskAuraIntent: AppIntent {

    static var title: LocalizedStringResource = "Спросить Ауру"
    static var description = IntentDescription("Отправить вопрос Ауре и получить ответ.")
    static var openAppWhenRun: Bool = false

    @Parameter(title: "Сообщение", requestValueDialog: "Что сказать Ауре?")
    var message: String

    func perform() async throws -> some IntentResult & ProvidesDialog & ReturnsValue<String> {
        let reply = try await IntentSession.shared.ask(message: message)
        return .result(value: reply, dialog: IntentDialog(stringLiteral: reply))
    }
}
