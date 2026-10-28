// OnboardingView.swift — анкета после регистрации (дизайн 2026): день рождения, аллергии,
// диета, передвижение, город, бюджет. Всё необязательно («Пропустить»), но
// заполненная анкета сразу попадает в контекст Selfvoy (prefs.set → AI context).
import SwiftUI

struct OnboardingView: View {

    @EnvironmentObject private var store: AppStore
    @State private var birthday = ""
    @State private var allergies = ""
    @State private var city = ""
    @State private var budget = ""
    @State private var diet = Set<String>()
    @State private var transport = "walk"

    private let dietOptions = [("vegan", "Веган"), ("vegetarian", "Вегетарианец"), ("gluten_free", "Без глютена"),
                               ("lactose_free", "Без лактозы"), ("halal", "Халяль"), ("kosher", "Кошер")]
    private let transportOptions = [("walk", "Пешком"), ("bike", "Велосипед"), ("transit", "Транспорт"),
                                    ("car", "Машина")]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                InkMark(size: 40)
                    .padding(.top, 24)
                VStack(alignment: .leading, spacing: 6) {
                    Text("Расскажите о себе")
                        .font(InkFont.largeTitle)
                        .foregroundStyle(Ink.label)
                        .accessibilityAddTraits(.isHeader)
                    Text("Selfvoy использует это для мест, времени и рекомендаций. Всё необязательно и меняется в настройках.")
                        .font(InkFont.callout)
                        .foregroundStyle(Ink.secondary)
                }
                InkField(text: $birthday, placeholder: "ГГГГ-ММ-ДД", label: "День рождения",
                         keyboard: .numbersAndPunctuation)
                InkField(text: $allergies, placeholder: "через запятую", label: "Аллергии")
                VStack(alignment: .leading, spacing: 8) {
                    Text("Диета").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
                    FlowLayout(spacing: 8) {
                        ForEach(dietOptions.indices, id: \.self) { index in
                            let option = dietOptions[index]
                            InkChip(title: option.1, active: diet.contains(option.0)) {
                                if diet.contains(option.0) { diet.remove(option.0) } else { diet.insert(option.0) }
                            }
                        }
                    }
                }
                VStack(alignment: .leading, spacing: 8) {
                    Text("Передвижение").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
                    FlowLayout(spacing: 8) {
                        ForEach(transportOptions.indices, id: \.self) { index in
                            let option = transportOptions[index]
                            InkChip(title: option.1, active: transport == option.0) { transport = option.0 }
                        }
                    }
                }
                InkField(text: $city, placeholder: "Например, Москва", label: "Город")
                InkField(text: $budget, placeholder: "Например, 25", label: "Бюджет на встречу", keyboard: .decimalPad)
                InkButton(title: "Сохранить", busy: store.busy) {
                    store.submitOnboarding(
                        birthday: birthday.trimmingCharacters(in: .whitespaces),
                        allergies: allergies.split(separator: ",")
                            .map { $0.trimmingCharacters(in: .whitespaces) }
                            .filter { !$0.isEmpty },
                        diet: dietOptions.map { $0.0 }.filter { diet.contains($0) },
                        transport: transport,
                        city: city.trimmingCharacters(in: .whitespaces),
                        budget: budget.trimmingCharacters(in: .whitespaces))
                }
                .padding(.top, 4)
                InkButton(title: "Пропустить", kind: .quiet) { store.skipOnboarding() }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 32)
            .frame(maxWidth: 560)
            .frame(maxWidth: .infinity)
        }
        .inkScreen()
        .scrollDismissesKeyboard(.interactively)
        .onAppear {
            let saved = store.prefs.string("city")
            if city.isEmpty, !saved.isEmpty { city = saved }
        }
    }
}
