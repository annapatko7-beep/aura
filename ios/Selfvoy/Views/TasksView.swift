// TasksView.swift — задачи (дизайн 2026): неделя, задачи выбранного дня,
// «Дальше», «Выполнено», подсказка «до следующего события» и «+» для новой
// задачи (tasks.create с due_at). Долгое нажатие — перенос в «Напоминания» iOS.
import SelfvoyKit
import SwiftUI

struct TasksView: View {

    @EnvironmentObject private var store: AppStore
    @State private var day = Calendar.current.startOfDay(for: Date())
    @State private var adding = false

    /// Совместимость с тестами и EventKit: ISO-строка сервера → Date.
    static func parseISO(_ text: String) -> Date? {
        Format.parseISO(text)
    }

    private var calendar: Calendar { Calendar.current }

    private var week: [Date] {
        var iso = Calendar(identifier: .iso8601)
        iso.timeZone = calendar.timeZone
        let monday = iso.dateInterval(of: .weekOfYear, for: day)?.start ?? day
        return (0..<7).compactMap { calendar.date(byAdding: .day, value: $0, to: monday) }
    }

    private var open: [SelfvoyTask] {
        store.tasks.filter { $0.status == "pending" }
            .sorted { ($0.whenAt.isEmpty ? "9999" : $0.whenAt) < ($1.whenAt.isEmpty ? "9999" : $1.whenAt) }
    }

    private func dateOf(_ task: SelfvoyTask) -> Date? {
        Format.parseISO(task.whenAt).map { calendar.startOfDay(for: $0) }
    }

    var body: some View {
        let onDay = open.filter { (dateOf($0) ?? calendar.startOfDay(for: Date())) == day }
        let later = open.filter { (dateOf($0) ?? .distantPast) > day }
        let done = Array(store.tasks.filter(\.isDone).prefix(6))
        let next = open.first { (Format.parseISO($0.whenAt) ?? .distantPast) > Date() }

        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "\(Format.dateLabel(week.first ?? day)) – \(Format.dateLabel(week.last ?? day))",
                          title: "Задачи") {
                    if !calendar.isDateInToday(day) {
                        Button("Сегодня") { day = calendar.startOfDay(for: Date()) }
                            .buttonStyle(InkSecondaryButtonStyle())
                    }
                    InkRoundButton(systemImage: "plus", label: "Новая задача") { adding = true }
                }
                weekStrip
                hint(next)
                section(calendar.isDateInToday(day) ? "Сегодня"
                        : "\(Format.dayShort(day)), \(Format.dateLabel(day))", onDay, empty: "Свободный день")
                section("Дальше", later, empty: "Больше ничего не запланировано")
                section("Выполнено", done, empty: "Пока ничего")
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .refreshable { store.loadTasks() }
        .sheet(isPresented: $adding) {
            AddTaskSheet(day: day)
                .inkSheet()
        }
    }

    /// Неделя: стрелки по краям, дни — выбранный на акценте, сегодня — акцентной цифрой.
    private var weekStrip: some View {
        HStack(spacing: 2) {
            weekArrow(systemName: "chevron.left", label: "Предыдущая неделя", days: -7)
            ForEach(week, id: \.self) { date in
                let selected = date == day
                let today = calendar.isDateInToday(date)
                let has = open.contains { dateOf($0) == date }
                Button {
                    day = date
                } label: {
                    VStack(spacing: 3) {
                        Text(Format.dayShort(date))
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(selected ? Color.white.opacity(0.85) : Ink.secondary)
                        Text("\(calendar.component(.day, from: date))")
                            .font(.system(size: 18, weight: .bold).monospacedDigit())
                            .foregroundStyle(selected ? Color.white : today ? Ink.accent : Ink.label)
                        Circle()
                            .fill(has ? (selected ? Color.white : Ink.accent) : Color.clear)
                            .frame(width: 5, height: 5)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
                    .background(RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(selected ? Ink.accent : Color.clear))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(Format.dayShort(date)), \(Format.dateLabel(date))"
                                    + (today ? ", сегодня" : "") + (has ? ", есть задачи" : ""))
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
            weekArrow(systemName: "chevron.right", label: "Следующая неделя", days: 7)
        }
        .padding(4)
        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Ink.card))
    }

    private func weekArrow(systemName: String, label: String, days: Int) -> some View {
        Button {
            day = calendar.date(byAdding: .day, value: days, to: day) ?? day
        } label: {
            Image(systemName: systemName)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Ink.secondary)
                .frame(width: 22, height: 44)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }

    /// Подсказка Selfvoy: сколько осталось до следующего события.
    private func hint(_ next: SelfvoyTask?) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                InkMark(size: 22)
                Text("Подсказка Selfvoy").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
            }
            Text(next.map { "«\($0.title)» \(Format.untilText($0.whenAt))" } ?? "Впереди ничего не запланировано.")
                .font(InkFont.headline)
                .foregroundStyle(Ink.label)
            if let next, let at = Format.parseISO(next.whenAt) {
                let minutes = at.timeIntervalSinceNow / 60
                let progress = min(1, max(0.05, 1 - minutes / 120))
                ProgressView(value: progress)
                    .tint(minutes < 30 ? Ink.amber : Ink.accent)
                Text("Время до следующего события")
                    .font(InkFont.caption)
                    .foregroundStyle(Ink.secondary)
            }
        }
        .inkCard()
    }

    @ViewBuilder
    private func section(_ title: String, _ items: [SelfvoyTask], empty: String) -> some View {
        InkSectionHeader(title: title, trailing: items.isEmpty ? "" : "\(items.count)")
        if items.isEmpty {
            InkEmpty(text: empty)
        } else {
            InkGroup {
                ForEach(Array(items.enumerated()), id: \.element.id) { index, task in
                    if index > 0 { InkDivider(inset: 52) }
                    TaskRow(task: task, day: day)
                }
            }
        }
    }
}

private struct TaskRow: View {

    @EnvironmentObject private var store: AppStore
    let task: SelfvoyTask
    let day: Date

    /// Срок словом и значком (не только цветом): просрочено, скоро или выполнено.
    private func dueStatus(_ at: Date?) -> StatusView? {
        if task.isDone { return StatusView("Выполнено", .green, .check) }
        guard let at else { return nil }
        if at < Date() { return StatusView("Просрочено", .red, .clock) }
        if at.timeIntervalSinceNow < 120 * 60 { return StatusView("Скоро", .attention, .clock) }
        return nil
    }

    var body: some View {
        let at = Format.parseISO(task.whenAt)
        HStack(spacing: 12) {
            Button {
                store.setTaskStatus(id: task.id, status: task.isDone ? "reopen" : "complete")
            } label: {
                Image(systemName: task.isDone ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 22))
                    .foregroundStyle(task.isDone ? Ink.accent : Ink.tertiary)
                    .frame(width: 28, height: 28)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(task.isDone ? "Вернуть в работу" : "Выполнено")
            VStack(alignment: .leading, spacing: 3) {
                Text(task.title)
                    .font(InkFont.body)
                    .foregroundStyle(task.isDone ? Ink.secondary : Ink.label)
                    .strikethrough(task.isDone)
                    .lineLimit(2)
                HStack(spacing: 6) {
                    if let at {
                        // День недели — только внутри показанной недели, иначе «11 октября».
                        let inWeek = Calendar(identifier: .iso8601).isDate(at, equalTo: day, toGranularity: .weekOfYear)
                        Text(Calendar.current.isDate(at, inSameDayAs: day)
                             ? Format.timeOf(task.whenAt)
                             : "\(inWeek ? Format.dayShort(at) : Format.dateLabel(at)), \(Format.timeOf(task.whenAt))")
                            .font(InkFont.footnote)
                            .foregroundStyle(Ink.secondary)
                    }
                    if let status = dueStatus(at) {
                        StatusBadge(status: status, compact: true)
                    }
                }
            }
            Spacer(minLength: 4)
            Button {
                store.deleteTask(id: task.id)
            } label: {
                Image(systemName: "trash")
                    .font(.system(size: 14))
                    .foregroundStyle(Ink.tertiary)
                    .frame(width: 32, height: 32)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Удалить")
        }
        .padding(.leading, 12)
        .padding(.trailing, 8)
        .padding(.vertical, 8)
        .frame(minHeight: 52)
        .contentShape(Rectangle())
        .contextMenu {
            Button {
                Task {
                    let error = await EventKitService.shared.exportTask(title: task.title, notes: task.notes,
                                                                        dueAt: Format.parseISO(task.whenAt))
                    if let error {
                        store.errorMessage = error
                    } else {
                        store.setStatus("Задача добавлена в «Напоминания»")
                    }
                }
            } label: {
                Label("В «Напоминания» iOS", systemImage: "checklist")
            }
        }
    }
}

/// Новая задача на выбранный день: название и время.
private struct AddTaskSheet: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss
    let day: Date
    @State private var title = ""
    @State private var time: Date

    init(day: Date) {
        self.day = day
        _time = State(initialValue: Calendar.current.date(bySettingHour: 18, minute: 0, second: 0, of: day) ?? day)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Button("Отмена") { dismiss() }
                        .buttonStyle(InkQuietButtonStyle())
                    Spacer()
                }
                Text("Новая задача · \(Format.dateLabel(day))")
                    .font(InkFont.title)
                    .foregroundStyle(Ink.label)
                InkField(text: $title, placeholder: "Например, отправить питч-дек", label: "Что сделать")
                DatePicker("Время", selection: $time, displayedComponents: .hourAndMinute)
                    .font(InkFont.body)
                    .foregroundStyle(Ink.label)
                    .inkRow(vertical: 6)
                    .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Ink.card))
                InkButton(title: "Добавить", enabled: !title.trimmingCharacters(in: .whitespaces).isEmpty) {
                    let parts = Calendar.current.dateComponents([.hour, .minute], from: time)
                    let due = Calendar.current.date(bySettingHour: parts.hour ?? 18, minute: parts.minute ?? 0,
                                                    second: 0, of: day) ?? time
                    store.createTask(title: title, dueAt: Format.isoString(due))
                    dismiss()
                }
            }
            .padding(20)
        }
        .inkScreen()
    }
}
