// CreateTaskIntent.swift — «Создать задачу» для Siri и Команд.
import AppIntents
import SelfvoyKit
import Foundation

struct CreateTaskIntent: AppIntent {

    static var title: LocalizedStringResource = "Создать задачу в Selfvoy"
    static var description = IntentDescription("Создать задачу/напоминание в Selfvoy.")
    static var openAppWhenRun: Bool = false

    @Parameter(title: "Задача", requestValueDialog: "Какую задачу создать?")
    var taskTitle: String

    // Необязательный параметр: без default (вариант с default требует iOS 18).
    @Parameter(title: "Напомнить (дата)", description: "Необязательно")
    var remindDate: Date?

    func perform() async throws -> some IntentResult & ProvidesDialog {
        var remindAt = ""
        if let remindDate {
            let formatter = ISO8601DateFormatter()
            formatter.formatOptions = [.withInternetDateTime]
            remindAt = formatter.string(from: remindDate)
        }
        _ = try await IntentSession.shared.createTask(title: taskTitle, remindAt: remindAt)
        let dialog: String
        if let remindDate {
            let spoken = DateFormatter()
            spoken.locale = Locale(identifier: "ru_RU")
            spoken.dateFormat = "d MMMM 'в' HH:mm"
            dialog = "Задача «\(taskTitle)» создана, напомню \(spoken.string(from: remindDate))"
        } else {
            dialog = "Задача «\(taskTitle)» создана"
        }
        return .result(dialog: IntentDialog(stringLiteral: dialog))
    }
}
