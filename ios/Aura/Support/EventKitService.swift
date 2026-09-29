// EventKitService.swift — экспорт задач Aura в системные «Напоминания» iOS.
//
// Разрешение запрашивается только в момент действия (кнопка «В напоминания
// iOS»): никаких фоновых обращений к EventKit. Официальный API, без excess
// permissions — NSRemindersFullAccessUsageDescription в Info.plist.
import EventKit
import Foundation

@MainActor
final class EventKitService {

    static let shared = EventKitService()

    private let store = EKEventStore()

    /// Добавляет задачу как напоминание iOS. Возвращает текст ошибки (nil = успех).
    func exportTask(title: String, notes: String, dueAt: Date?) async -> String? {
        do {
            let granted = try await store.requestFullAccessToReminders()
            guard granted else {
                return "нет доступа к напоминаниям — разрешите в Настройках iOS"
            }
            let reminder = EKReminder(eventStore: store)
            reminder.title = title
            if !notes.isEmpty { reminder.notes = notes }
            if let dueAt {
                reminder.dueDateComponents = Calendar.current.dateComponents(
                    [.year, .month, .day, .hour, .minute], from: dueAt)
                reminder.addAlarm(EKAlarm(date: dueAt))
            }
            try store.save(reminder, commit: true)
            return nil
        } catch {
            return error.localizedDescription
        }
    }
}
