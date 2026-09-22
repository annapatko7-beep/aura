// QuickActionsView.swift — честные инструкции по Back Tap.
//
// Back Tap — системный жест iOS: стороннее приложение не может перехватить
// его напрямую и не имитирует его. Мы подключаем жест к App Intent «Спросить
// Ауру» через «Команды» — официальный механизм Apple.
import SwiftUI

struct QuickActionsView: View {

    var body: some View {
        List {
            Section("Что доступно") {
                Label("Спросить Ауру", systemImage: "sparkles")
                Label("Создать задачу", systemImage: "checklist")
                Text("Команды появляются в приложении «Команды» автоматически "
                     + "после установки Aura — их можно запускать голосом "
                     + "(«Привет, Siri») или назначить на жест.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            Section("Back Tap: настройка за 5 шагов") {
                InstructionStep(number: 1,
                                text: "Откройте «Настройки» → «Универсальный доступ» → «Касание».")
                InstructionStep(number: 2,
                                text: "Прокрутите вниз и выберите «Касание сзади» (Back Tap).")
                InstructionStep(number: 3,
                                text: "Выберите «Двойное касание» или «Тройное касание».")
                InstructionStep(number: 4,
                                text: "В списке действий прокрутите до раздела «Команды» и выберите «Спросить Ауру».")
                InstructionStep(number: 5,
                                text: "Дважды (или трижды) постучите по спинке iPhone — Siri-диалог спросит сообщение, Аура ответит.")
            }

            Section("Важно") {
                Text("Back Tap — системный жест iOS, приложение не может "
                     + "перехватить его напрямую. Мы не имитируем жест: "
                     + "используется только официальный механизм — команда "
                     + "Apple, назначенная на Back Tap.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Link("Открыть приложение «Команды»",
                     destination: URL(string: "shortcuts://")!)
            }
        }
        .navigationTitle("Quick Actions")
    }
}

private struct InstructionStep: View {

    let number: Int
    let text: String

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text("\(number)")
                .font(.headline)
                .frame(width: 26, height: 26)
                .background(Color.accentColor.opacity(0.2), in: Circle())
            Text(text)
                .font(.subheadline)
        }
        .padding(.vertical, 2)
    }
}
