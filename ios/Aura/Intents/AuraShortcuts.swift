// AuraShortcuts.swift — фразы Siri (App Shortcuts).
//
// После сборки приложения команды автоматически появляются в «Команды» —
// их можно назначить на Back Tap, кнопку действия или автоматизации.
import AppIntents

struct AuraShortcuts: AppShortcutsProvider {

    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: AskAuraIntent(),
            phrases: [
                "Спросить Ауру в \(.applicationName): \{message}",
                "В \(.applicationName) спроси Ауру \{message}"
            ],
            shortTitle: "Спросить Ауру",
            systemImageName: "sparkles"
        )
        AppShortcut(
            intent: CreateTaskIntent(),
            phrases: [
                "Создать задачу в \(.applicationName): \{taskTitle}",
                "В \(.applicationName) новая задача \{taskTitle}"
            ],
            shortTitle: "Создать задачу",
            systemImageName: "checklist"
        )
    }
}
