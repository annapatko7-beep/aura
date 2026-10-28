// SelfvoyShortcuts.swift — фразы Siri (App Shortcuts).
//
// После сборки приложения команды автоматически появляются в «Команды» —
// их можно назначить на Back Tap, кнопку действия или автоматизации.
// В фразах допустимы только параметры AppEntity/AppEnum, поэтому текст
// запроса и название задачи Siri спрашивает отдельно (requestValueDialog).
import AppIntents

struct SelfvoyShortcuts: AppShortcutsProvider {

    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: AskSelfvoyIntent(),
            phrases: [
                "Спросить Selfvoy в \(.applicationName)",
                "В \(.applicationName) спроси Selfvoy"
            ],
            shortTitle: "Спросить Selfvoy",
            systemImageName: "sparkles"
        )
        AppShortcut(
            intent: CreateTaskIntent(),
            phrases: [
                "Создать задачу в \(.applicationName)",
                "В \(.applicationName) новая задача"
            ],
            shortTitle: "Создать задачу",
            systemImageName: "checklist"
        )
    }
}
