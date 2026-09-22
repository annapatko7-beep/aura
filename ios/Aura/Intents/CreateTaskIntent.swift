// CreateTaskIntent.swift — «Создать задачу» для Siri и Команд.
import AppIntents
import AuraKit
import Foundation

struct CreateTaskIntent: AppIntent {

    static var title: LocalizedStringResource = "Создать задачу в Ауре"
    static var description = IntentDescription("Создать задачу/напоминание в Aura.")
    static var openAppWhenRun: Bool = false

    @Parameter(title: "Задача", requestValueDialog: "Какую задачу создать?")
    var taskTitle: String

    @Parameter(title: "Напомнить (дата)", description: "Необязательно", default: Date?.none)
    var remindDate: Date?

    func perform() async throws -> some IntentResult & ProvidesDialog {
        var remindAt = ""
        if let remindDate {
            let formatter = ISO8601DateFormatter()
            formatter.formatOptions = [.withInternetDateTime]
            remindAt = formatter.string(from: remindDate)
        }
        _ = try await IntentSession.shared.createTask(title: taskTitle, remindAt: remindAt)
        let dialog = remindAt.isEmpty
            ? "Задача «\(taskTitle)» создана"
            : "Задача «\(taskTitle)» создана, напомню \(remindAt)"
        return .result(dialog: IntentDialog(stringLiteral: dialog))
    }
}
