// TasksView.swift — задачи и напоминания (+ экспорт в напоминания iOS).
import SwiftUI

struct TasksView: View {

    @EnvironmentObject private var store: AppStore
    @State private var showNew = false
    @State private var newTitle = ""
    @State private var newNotes = ""
    @State private var newRemind = Date().addingTimeInterval(3600)
    @State private var remindEnabled = false
    @State private var exportMessage = ""

    var body: some View {
        NavigationStack {
            List {
                ForEach(store.tasks.indices, id: \.self) { index in
                    let task = store.tasks[index]
                    let pending = task.string("status") == "pending"
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Text(task.string("title"))
                                .strikethrough(!pending)
                            Spacer()
                            Text(task.string("status"))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        if !task.string("notes").isEmpty {
                            Text(task.string("notes"))
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                        if !task.string("remind_at").isEmpty {
                            Label(task.string("remind_at"), systemImage: "clock")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        if pending {
                            HStack(spacing: 16) {
                                Button("Готово") {
                                    store.setTaskStatus(id: task.int("id"), status: "complete")
                                }
                                Button("Отменить") {
                                    store.setTaskStatus(id: task.int("id"), status: "cancel")
                                }
                                Button("В напоминания iOS") {
                                    export(task: task)
                                }
                                Button(role: .destructive) {
                                    store.deleteTask(id: task.int("id"))
                                } label: {
                                    Image(systemName: "trash")
                                }
                            }
                            .font(.footnote)
                        }
                    }
                    .padding(.vertical, 2)
                }
            }
            .navigationTitle("Задачи")
            .toolbar {
                Button {
                    showNew = true
                } label: {
                    Image(systemName: "plus")
                }
            }
            .refreshable { store.loadTasks() }
            .alert("Новая задача", isPresented: $showNew) {
                TextField("Название", text: $newTitle)
                TextField("Заметки", text: $newNotes)
                Toggle("Напомнить", isOn: $remindEnabled)
                if remindEnabled {
                    DatePicker("Когда", selection: $newRemind)
                }
                Button("Создать") {
                    var remindAt = ""
                    if remindEnabled {
                        let formatter = ISO8601DateFormatter()
                        formatter.formatOptions = [.withInternetDateTime]
                        remindAt = formatter.string(from: newRemind)
                    }
                    store.createTask(title: newTitle, notes: newNotes, remindAt: remindAt)
                    newTitle = ""
                    newNotes = ""
                }
                Button("Отмена", role: .cancel) {}
            }
            .alert("Напоминания iOS", isPresented: .init(
                get: { !exportMessage.isEmpty },
                set: { if !$0 { exportMessage = "" } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(exportMessage)
            }
        }
    }

    private func export(task: JSONValue) {
        Task {
            let due = Self.parseISO(task.string("remind_at"))
            let error = await EventKitService.shared.exportTask(
                title: task.string("title"),
                notes: task.string("notes"),
                dueAt: due)
            exportMessage = error ?? "Добавлено в напоминания iOS"
        }
    }

    static func parseISO(_ text: String) -> Date? {
        guard !text.isEmpty else { return nil }
        let iso = ISO8601DateFormatter()
        iso.formatOptions = [.withInternetDateTime]
        if let date = iso.date(from: text) { return date }
        iso.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return iso.date(from: text)
    }
}
