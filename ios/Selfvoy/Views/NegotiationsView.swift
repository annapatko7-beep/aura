// NegotiationsView.swift — переговоры Selfvoy (дизайн 2026).
//
// Один agent.negotiate — один раунд; шаги строятся из ответа сервера и
// появляются по очереди. Бронь — только по кнопке пользователя. После пяти
// раундов без согласия решает человек (docs/A2A_PROTOCOL.md).
import SelfvoyKit
import SwiftUI

private struct Step: Hashable {
    let mine: Bool
    let text: String
    let detail: String
    var done = false
    var fail = false
}

struct NegotiationsView: View {

    @EnvironmentObject private var store: AppStore
    @State private var composing = false
    @State private var revealed = 0

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Selfvoy договаривается с Selfvoy", title: "Переговоры") {
                    if store.negotiation != nil && !composing {
                        InkRoundButton(systemImage: "plus", label: "Новые переговоры") { composing = true }
                    }
                }
                // Страница записи: постоянная ссылка для людей без Selfvoy.
                InkGroup {
                    InkCell(symbol: "calendar.badge.plus", tint: Ink.accent,
                            title: "Страница записи — пусть выбирают время сами") {
                        store.selectTab(.booking)
                    }
                }
                if let n = store.negotiation, !composing {
                    progress(n)
                } else {
                    NegotiationForm(onCancel: store.negotiation == nil ? nil : { composing = false }) {
                        composing = false
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .scrollDismissesKeyboard(.interactively)
    }

    @ViewBuilder
    private func progress(_ n: Negotiation) -> some View {
        let me = Format.firstName(store.userName)
        let contact = store.contacts.first { $0.email == n.target || $0.name == n.target }
        let them = Format.firstName(contact?.name ?? n.target)
        let steps = buildSteps(n, me: me, them: them)
        let shown = store.revealedNegotiations.contains(n.id) ? steps.count : revealed

        HStack(spacing: 8) {
            StatusBadge(status: status(n))
            StatusBadge(status: StatusView("Раунд \(n.round) из \(AppStore.maxRounds)", .neutral, .clock))
        }
        HStack(spacing: 10) {
            AgentCard(name: me, selfvoy: "Selfvoy \(Format.genitive(me))",
                      talking: shown < steps.count && steps[shown].mine, them: false)
            AgentCard(name: them, selfvoy: "Selfvoy \(Format.genitive(them))",
                      talking: shown < steps.count && !steps[shown].mine, them: true)
        }
        if !n.topic.isEmpty {
            Text("Тема: \(n.topic)")
                .font(InkFont.callout)
                .foregroundStyle(Ink.secondary)
                .frame(maxWidth: .infinity)
        }
        VStack(spacing: 8) {
            ForEach(Array(steps.prefix(shown).enumerated()), id: \.offset) { index, step in
                StepRow(number: index + 1, step: step)
                    .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .task(id: n.id) {
            guard !store.revealedNegotiations.contains(n.id) else { return }
            revealed = 0
            while revealed < steps.count {
                try? await Task.sleep(nanoseconds: 800_000_000)
                if Task.isCancelled { return }
                withAnimation(.easeOut(duration: 0.3)) { revealed += 1 }
            }
            store.revealedNegotiations.insert(n.id)
        }
        if shown >= steps.count {
            if n.accepted, n.slot != nil {
                ResultCard(negotiation: n)
            } else {
                counterOffers(n)
            }
        }
        if store.negotiationHistory.count > 1 {
            InkSectionHeader(title: "Раунды за сессию")
            InkGroup {
                ForEach(Array(store.negotiationHistory.dropFirst().enumerated()), id: \.element.id) { index, h in
                    if index > 0 { InkDivider(inset: 16) }
                    HStack(spacing: 10) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(h.target).font(InkFont.body).foregroundStyle(Ink.label).lineLimit(1)
                            Text(historyLine(h))
                                .font(InkFont.footnote)
                                .foregroundStyle(Ink.secondary)
                        }
                        Spacer(minLength: 8)
                        StatusBadge(status: h.accepted ? StatusView("Согласовано", .green, .check)
                                                       : StatusView("Без согласия", .neutral, .cross),
                                    compact: true)
                    }
                    .inkRow()
                }
            }
        }
    }

    /// «Раунд 2 · Кафе Пушкин» — место только у согласованного раунда.
    private func historyLine(_ h: Negotiation) -> String {
        let place = h.place?.name ?? ""
        return h.accepted && !place.isEmpty ? "Раунд \(h.round) · \(place)" : "Раунд \(h.round)"
    }

    /// Статус переговоров — значком и словом.
    private func status(_ n: Negotiation) -> StatusView {
        if store.busy { return StatusView("Идут", .accent, .clock) }
        if n.booking != nil { return StatusView("Забронировано", .green, .check) }
        return n.accepted ? StatusView("Ждёт решения", .attention, .hand)
                          : StatusView("Нужен ваш выбор", .attention, .hand)
    }

    private func counterOffers(_ n: Negotiation) -> some View {
        let exhausted = n.round >= AppStore.maxRounds
        return VStack(alignment: .leading, spacing: 12) {
            Text(exhausted ? "Лимит раундов исчерпан" : "Встречные варианты собеседника")
                .font(InkFont.headline)
                .foregroundStyle(Ink.label)
            Text(exhausted
                 ? "Оба Selfvoy провели \(AppStore.maxRounds) раундов без согласия. Дальше решаете вы: напишите собеседнику."
                 : "Без вас Selfvoy дальше не договаривается: можно начать следующий раунд с другим окном или написать в чат.")
                .font(InkFont.footnote)
                .foregroundStyle(Ink.secondary)
            if !n.counterSlots.isEmpty {
                FlowLayout(spacing: 8) {
                    ForEach(n.counterSlots, id: \.self) { slot in
                        InkChip(title: "\(Format.dayPhrase(slot.start)), \(Format.timeOf(slot.start))",
                                enabled: false) {}
                    }
                }
            }
            if !exhausted {
                InkButton(title: "Следующий раунд: две недели", busy: store.busy) {
                    store.negotiate(target: n.target, topic: n.topic, durationMinutes: n.durationMinutes,
                                    windowHours: Format.windowHours("week"))
                }
            }
            InkButton(title: "Написать собеседнику", systemImage: "bubble.left.fill", kind: .secondary) {
                store.openChat(contact: n.target)
            }
        }
        .inkCard()
    }

    private func buildSteps(_ n: Negotiation, me: String, them: String) -> [Step] {
        let selfvoyMe = "Selfvoy \(Format.genitive(me))"
        let selfvoyThem = "Selfvoy \(Format.genitive(them))"
        var steps = [Step(mine: true, text: "\(selfvoyMe) предложил встречу: \(n.durationMinutes) мин",
                          detail: "окно: \(Format.windowPhrase(n.windowHours))")]
        if n.accepted, let slot = n.slot {
            steps.append(Step(mine: false, text: "\(selfvoyThem) проверил календарь",
                              detail: "общее окно: \(Format.dayPhrase(slot.start)) в \(Format.timeOf(slot.start))"))
            var facts: [String] = []
            if let place = n.place {
                if !place.city.isEmpty { facts.append(place.city) }
                if !place.tags.isEmpty { facts.append(Format.placeTags(place.tags)) }
                if !place.rating.isEmpty { facts.append("★ \(place.rating)") }
            }
            steps.append(Step(mine: false, text: "\(selfvoyThem) предложил \(n.place?.name ?? "место")",
                              detail: facts.joined(separator: " · ")))
            steps.append(Step(mine: true, text: "\(selfvoyMe) принял место и время", detail: n.message, done: true))
        } else {
            steps.append(Step(mine: false, text: "\(selfvoyThem) проверил календарь",
                              detail: "в этом окне общего времени нет"))
            steps.append(Step(mine: false,
                              text: n.counterSlots.isEmpty ? "Встречных вариантов нет"
                                  : "Встречные варианты: \(n.counterSlots.count)",
                              detail: n.message.isEmpty ? "нужно ваше решение" : n.message, fail: true))
        }
        return steps
    }
}

/// Карточка агента: аватар и «голос» — полосы выше у того, кто говорит; рядом слово.
private struct AgentCard: View {
    let name: String
    let selfvoy: String
    let talking: Bool
    let them: Bool

    var body: some View {
        VStack(spacing: 4) {
            InkAvatar(name: name, size: 52)
                .padding(.bottom, 4)
            Text(name).font(InkFont.headline).foregroundStyle(Ink.label).lineLimit(1)
            Text(selfvoy).font(InkFont.caption).foregroundStyle(Ink.secondary).lineLimit(1)
            HStack(alignment: .bottom, spacing: 2) {
                ForEach(0..<16, id: \.self) { i in
                    let level = 0.25 + abs(sin(Double(i) * 1.7 + (them ? 2 : 0))) * 0.65
                    RoundedRectangle(cornerRadius: 1.5)
                        .fill(talking ? Ink.accent : Ink.tertiary)
                        .frame(height: 28 * (talking ? level : level * 0.35))
                        .animation(.easeInOut(duration: 0.35), value: talking)
                }
            }
            .frame(height: 28, alignment: .bottom)
            .padding(.top, 6)
            .accessibilityHidden(true)
            Text(talking ? "говорит" : "ждёт")
                .font(InkFont.caption)
                .foregroundStyle(talking ? Ink.accent : Ink.secondary)
        }
        .padding(14)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Ink.card))
        .accessibilityElement(children: .combine)
    }
}

private struct StepRow: View {
    let number: Int
    let step: Step

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            if step.mine { badge } else { Spacer(minLength: 24) }
            VStack(alignment: step.mine ? .leading : .trailing, spacing: 2) {
                Text(step.text).font(InkFont.callout.weight(.semibold)).foregroundStyle(Ink.label)
                if !step.detail.isEmpty {
                    Text(step.detail).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
            }
            .multilineTextAlignment(step.mine ? .leading : .trailing)
            .padding(.horizontal, 14)
            .padding(.vertical, 9)
            .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Ink.card))
            if step.mine { Spacer(minLength: 24) } else { badge }
        }
    }

    /// Номер шага; итог — значком: галочка (согласовано) или крестик (без согласия).
    private var badge: some View {
        ZStack {
            Circle().fill(step.fail ? Ink.red.opacity(0.14) : step.done ? Ink.green.opacity(0.16) : Ink.accentSoft)
            if step.done {
                Image(systemName: "checkmark").font(.system(size: 13, weight: .bold)).foregroundStyle(Ink.green)
            } else if step.fail {
                Image(systemName: "xmark").font(.system(size: 13, weight: .bold)).foregroundStyle(Ink.red)
            } else {
                Text("\(number)").font(.system(size: 14, weight: .bold)).foregroundStyle(Ink.accent)
            }
        }
        .frame(width: 30, height: 30)
        .accessibilityLabel(step.done ? "Согласовано" : step.fail ? "Без согласия" : "Шаг \(number)")
    }
}

/// Итог: день, время, место и «Подтвердить и забронировать».
private struct ResultCard: View {

    @EnvironmentObject private var store: AppStore
    let negotiation: Negotiation

    var body: some View {
        let n = negotiation
        let slot = n.slot ?? Slot(start: "", end: "")
        var facts = ["Двое", "до \(Format.timeOf(slot.end))"]
        if let city = n.place?.city, !city.isEmpty { facts.append(city) }
        if let rating = n.place?.rating, !rating.isEmpty { facts.append("рейтинг \(rating)") }
        return VStack(alignment: .leading, spacing: 10) {
            Text("Общее время").font(InkFont.eyebrow).foregroundStyle(Ink.secondary).textCase(.uppercase)
            Text("\(Format.dayTitle(slot.start)), \(Format.timeOf(slot.start))")
                .font(InkFont.title)
                .foregroundStyle(Ink.label)
            if let place = n.place?.name, !place.isEmpty {
                Text(place).font(InkFont.headline).foregroundStyle(Ink.label)
            }
            Text(facts.joined(separator: " · ")).font(InkFont.footnote).foregroundStyle(Ink.secondary)
            if let booking = n.booking {
                StatusBadge(status: StatusView("Столик забронирован · код \(booking)", .green, .check))
                Text("Встреча — в задачах.").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                InkButton(title: "Открыть задачи", kind: .secondary) { store.selectTab(.tasks) }
            } else {
                InkButton(title: "Подтвердить и забронировать", systemImage: "checkmark.seal.fill",
                          busy: store.busy) {
                    store.bookNegotiation()
                }
                InkButton(title: "Остановить", kind: .quiet) { store.clearNegotiation() }
            }
        }
        .inkCard(padding: 18)
    }
}

/// Форма нового раунда: с кем, о чём, когда и сколько.
private struct NegotiationForm: View {

    @EnvironmentObject private var store: AppStore
    let onCancel: (() -> Void)?
    let onStart: () -> Void

    @State private var target = ""
    @State private var email = ""
    @State private var topic = ""
    @State private var window = "weekend"
    @State private var duration = 60

    private struct WindowOption {
        let key: String
        let title: String
    }

    private static let windows = [
        WindowOption(key: "tomorrow", title: "сегодня–завтра"),
        WindowOption(key: "weekend", title: "до конца недели"),
        WindowOption(key: "week", title: "две недели")
    ]

    init(onCancel: (() -> Void)?, onStart: @escaping () -> Void) {
        self.onCancel = onCancel
        self.onStart = onStart
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("С кем договориться?").font(InkFont.headline).foregroundStyle(Ink.label)
            if !store.contacts.isEmpty {
                FlowLayout(spacing: 8) {
                    ForEach(store.contacts) { contact in
                        InkChip(title: contact.name, active: target == contact.email && email.isEmpty) {
                            target = contact.email
                            email = ""
                        }
                    }
                }
            }
            InkField(text: $email, placeholder: "или email собеседника", systemImage: "envelope",
                     inCard: true, keyboard: .emailAddress)
                .onChange(of: email) {
                    if !email.trimmingCharacters(in: .whitespaces).isEmpty { target = email }
                }
            InkField(text: $topic, placeholder: "Тема, например «обсудить стартап»", inCard: true)
            VStack(alignment: .leading, spacing: 8) {
                Text("Когда").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
                FlowLayout(spacing: 8) {
                    ForEach(Self.windows, id: \.key) { option in
                        InkChip(title: option.title, active: window == option.key) { window = option.key }
                    }
                }
                Text("Длительность").font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
                    .padding(.top, 4)
                Picker("Длительность", selection: $duration) {
                    ForEach([30, 60, 90], id: \.self) { minutes in
                        Text("\(minutes) мин").tag(minutes)
                    }
                }
                .pickerStyle(.segmented)
            }
            InkButton(title: "Начать переговоры", systemImage: "person.2.fill",
                      busy: store.busy, enabled: !target.isEmpty && !store.busy) {
                store.negotiate(target: target, topic: topic, durationMinutes: duration,
                                windowHours: Format.windowHours(window))
                onStart()
            }
            if let onCancel {
                InkButton(title: "Отмена", kind: .quiet, action: onCancel)
            }
            InkFootnote("Оба Selfvoy сверят свободное время и подберут место с учётом питания обоих. Бронь только после вашего подтверждения.")
        }
        .inkCard(padding: 18)
        .onAppear {
            if target.isEmpty, let first = store.contacts.first { target = first.email }
        }
    }
}
