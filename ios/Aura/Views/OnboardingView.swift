// OnboardingView.swift — опрос после регистрации: день рождения, аллергии,
// диета, город, транспорт, бюджет. Всё необязательное; заполненные поля
// уходят в prefs.set и попадают в контекст Ауры. Флаг onboarded ставит
// сервер (docs/PROTOCOL.md) — до тех пор окно показывается после входа.
import SwiftUI

struct OnboardingView: View {

    @EnvironmentObject private var store: AppStore

    @State private var birthday = ""
    @State private var allergies = ""
    @State private var city = ""
    @State private var budget = ""
    @State private var diet: Set<String> = []
    @State private var transport = "walk"

    private let dietOptions = ["vegan", "vegetarian", "gluten_free", "lactose_free", "halal", "kosher"]
    private let transportOptions = [("walk", "Пешком"), ("bike", "Велосипед"),
                                    ("transit", "Транспорт"), ("car", "Машина")]

    var body: some View {
        NavigationStack {
            Form {
                Section("О вас") {
                    TextField("День рождения (ГГГГ-ММ-ДД)", text: $birthday)
                        .keyboardType(.numbersAndPunctuation)
                        .autocorrectionDisabled()
                    TextField("Аллергии (через запятую)", text: $allergies)
                    TextField("Город", text: $city)
                    TextField("Бюджет на встречу, €", text: $budget)
                        .keyboardType(.decimalPad)
                }
                Section("Диета") {
                    chips(dietOptions, selection: $diet, multi: true) { dietLabel($0) }
                }
                Section("Передвижение") {
                    chips(transportOptions.map { $0.0 }, selection: Binding(
                        get: { [transport] },
                        set: { transport = $0.first ?? "walk" }
                    ), multi: false) { value in
                        transportOptions.first(where: { $0.0 == value })?.1 ?? value
                    }
                }
                Section {
                    Button("Сохранить") {
                        store.submitOnboarding(
                            birthday: birthday.trimmingCharacters(in: .whitespaces),
                            allergies: allergies.split(separator: ",")
                                .map { $0.trimmingCharacters(in: .whitespaces) }
                                .filter { !$0.isEmpty },
                            diet: Array(diet),
                            transport: transport,
                            city: city.trimmingCharacters(in: .whitespaces),
                            budget: budget.trimmingCharacters(in: .whitespaces)
                        )
                    }
                    Button("Пропустить", role: .cancel) { store.skipOnboarding() }
                } footer: {
                    Text("Аура использует это для мест, времени и рекомендаций. Изменить можно в настройках.")
                }
            }
            .navigationTitle("Расскажите о себе")
            .interactiveDismissDisabled()
        }
    }

    // MARK: - Хелперы

    @ViewBuilder
    private func chips(_ options: [String], selection: Binding<Set<String>>,
                       multi: Bool, label: @escaping (String) -> String) -> some View {
        // Простая сетка «чипов» на кнопках — без сторонних зависимостей.
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 120))], alignment: .leading) {
            ForEach(options, id: \.self) { option in
                let isOn = selection.wrappedValue.contains(option)
                Button {
                    if multi {
                        if isOn { selection.wrappedValue.remove(option) }
                        else { selection.wrappedValue.insert(option) }
                    } else {
                        selection.wrappedValue = [option]
                    }
                } label: {
                    Text(label(option))
                        .padding(.horizontal, 12).padding(.vertical, 6)
                        .background(isOn ? Color.accentColor.opacity(0.25) : Color.gray.opacity(0.12),
                                    in: Capsule())
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func dietLabel(_ option: String) -> String {
        switch option {
        case "vegan": return "Веган"
        case "vegetarian": return "Вегетарианец"
        case "gluten_free": return "Без глютена"
        case "lactose_free": return "Без лактозы"
        case "halal": return "Халяль"
        case "kosher": return "Кошер"
        default: return option
        }
    }
}
