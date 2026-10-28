// MySelfvoyView.swift — «ИИ»: диалог с ассистентом (дизайн 2026). Ответы Selfvoy —
// слева на карточке, реплики пользователя — справа на акценте. Опасные действия
// (письмо, сообщение, бронь) Selfvoy сам не выполняет: под ответом карточка
// «Ждёт решения» с кнопками «Подтвердить / Отклонить».
import SelfvoyKit
import SwiftUI

struct MySelfvoyView: View {

    @EnvironmentObject private var store: AppStore
    @State private var draft = ""
    @State private var showMemory = false

    private let suggestions = [
        "Хочу встретиться с Димой в эти выходные и обсудить стартап",
        "Что у меня сегодня в расписании?",
        "Напомни завтра в 10 позвонить юристу"
    ]

    var body: some View {
        VStack(spacing: 0) {
            InkHeader(eyebrow: store.busy ? "Думает…" : "Персональный ассистент", title: "Ассистент") {
                InkRoundButton(systemImage: "brain", label: "Память") {
                    store.loadMemory()
                    showMemory = true
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 4)
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 12) {
                        if store.dialog.isEmpty { intro }
                        ForEach(store.dialog) { entry in
                            DialogRow(entry: entry, pending: Set(store.confirmations.map(\.id)))
                                .id(entry.id)
                        }
                        if store.busy && store.dialog.last?.fromSelfvoy == false {
                            HStack(spacing: 8) {
                                ProgressView().controlSize(.small)
                                Text("Selfvoy думает…").font(InkFont.footnote).foregroundStyle(Ink.secondary)
                            }
                            .padding(.leading, 4)
                            .id("thinking")
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 12)
                }
                .scrollDismissesKeyboard(.interactively)
                .onChange(of: store.dialog.count) {
                    if let last = store.dialog.last {
                        withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
            }
            ChatComposer(draft: $draft, placeholder: "Напишите или скажите…",
                         canSend: !draft.trimmingCharacters(in: .whitespaces).isEmpty && !store.busy,
                         onVoice: { store.voiceOverlayVisible = true },
                         onSend: { send(draft) })
        }
        .inkScreen()
        .sheet(isPresented: $showMemory) {
            MemorySheet()
                .inkSheet()
        }
    }

    private var intro: some View {
        VStack(alignment: .leading, spacing: 10) {
            Bubble(mine: false,
                   text: "Привет, \(Format.firstName(store.userName))! Я ваш Selfvoy. Скажите, что нужно сделать, "
                       + "и я договорюсь с Selfvoy других людей, поставлю напоминание или подготовлю письмо.")
            FlowLayout(spacing: 8) {
                ForEach(suggestions, id: \.self) { text in
                    InkChip(title: text) { send(text) }
                }
            }
        }
    }

    private func send(_ text: String) {
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty, !store.busy else { return }
        store.ask(text)
        draft = ""
    }
}

/// Реплика диалога с карточкой решения под ответом Selfvoy.
private struct DialogRow: View {

    @EnvironmentObject private var store: AppStore
    let entry: DialogEntry
    let pending: Set<Int64>

    var body: some View {
        HStack {
            if !entry.fromSelfvoy { Spacer(minLength: 40) }
            Bubble(mine: !entry.fromSelfvoy, text: entry.text, error: entry.error) {
                ForEach(entry.actions, id: \.confirmationId) { action in
                    let open = pending.contains(action.confirmationId)
                    VStack(alignment: .leading, spacing: 8) {
                        StatusBadge(status: open ? StatusView("Ждёт вашего решения", .attention, .hand)
                                                 : StatusView("Решено", .neutral, .check),
                                    compact: true)
                        Text(action.summary).font(InkFont.callout).foregroundStyle(Ink.label)
                        if open {
                            HStack(spacing: 8) {
                                InkButton(title: "Подтвердить", kind: .secondary) {
                                    store.decideConfirmation(id: action.confirmationId, approve: true)
                                }
                                InkButton(title: "Отклонить", kind: .destructive) {
                                    store.decideConfirmation(id: action.confirmationId, approve: false)
                                }
                            }
                        }
                    }
                    .padding(12)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Ink.fill.opacity(0.6)))
                }
                if entry.hasA2a {
                    InkButton(title: "Открыть переговоры", systemImage: "person.2.fill", kind: .secondary,
                              fullWidth: false) {
                        store.selectTab(.negotiations)
                    }
                }
            }
            if entry.fromSelfvoy { Spacer(minLength: 40) }
        }
    }
}

/// Реплика: своя — справа на акценте, собеседника и Selfvoy — слева на карточке.
struct Bubble<Extra: View>: View {
    let mine: Bool
    let text: String
    var error = false
    @ViewBuilder var extra: () -> Extra

    init(mine: Bool, text: String, error: Bool = false,
         @ViewBuilder extra: @escaping () -> Extra = { EmptyView() }) {
        self.mine = mine
        self.text = text
        self.error = error
        self.extra = extra
    }

    var body: some View {
        let shape = UnevenRoundedRectangle(topLeadingRadius: 18,
                                           bottomLeadingRadius: mine ? 18 : 6,
                                           bottomTrailingRadius: mine ? 6 : 18,
                                           topTrailingRadius: 18,
                                           style: .continuous)
        VStack(alignment: .leading, spacing: 10) {
            if error {
                Label("Не получилось", systemImage: "exclamationmark.triangle.fill")
                    .font(InkFont.caption)
                    .foregroundStyle(Ink.red)
            }
            Text(text)
                .font(InkFont.body)
                .foregroundStyle(mine ? Color.white : Ink.label)
                .fixedSize(horizontal: false, vertical: true)
            extra()
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(maxWidth: 320, alignment: .leading)
        .background(shape.fill(mine ? Ink.accent : Ink.card))
        .overlay {
            if error { shape.stroke(Ink.red.opacity(0.6), lineWidth: 1) }
        }
    }
}

/// Строка ввода внизу диалога: поле-капсула, голос (если есть) и «Отправить».
struct ChatComposer: View {
    @Binding var draft: String
    let placeholder: String
    /// Значок «Отправить»; nil — стрелка вверх.
    var sendSymbol: String?
    let canSend: Bool
    var onVoice: (() -> Void)?
    let onSend: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            if let onVoice {
                Button(action: onVoice) {
                    Image(systemName: "mic.fill")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(Ink.accent)
                        .frame(width: 40, height: 40)
                        .background(Circle().fill(Ink.card))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Сказать голосом")
            }
            TextField(placeholder, text: $draft)
                .font(InkFont.body)
                .foregroundStyle(Ink.label)
                .submitLabel(.send)
                .onSubmit(onSend)
                .padding(.horizontal, 14)
                .frame(minHeight: 40)
                .background(Capsule().fill(Ink.card))
                .overlay(Capsule().strokeBorder(Ink.separator, lineWidth: 0.5))
            Button(action: onSend) {
                Image(systemName: sendSymbol ?? "arrow.up")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 40, height: 40)
                    .background(Circle().fill(Ink.accent))
            }
            .buttonStyle(.plain)
            .disabled(!canSend)
            .opacity(canSend ? 1 : 0.4)
            .accessibilityLabel("Отправить")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Ink.bg)
    }
}

/// «Что Selfvoy знает о вас»: город из анкеты и факты из разговоров.
private struct MemorySheet: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let city = store.prefs.string("city")
        let facts = Array(store.memory.prefix(12))
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("Что Selfvoy знает о вас").font(InkFont.title).foregroundStyle(Ink.label)
                    Spacer(minLength: 8)
                    Button("Готово") { dismiss() }
                        .buttonStyle(InkQuietButtonStyle())
                }
                InkGroupHeader(title: "Город")
                InkGroup {
                    Text(city.isEmpty ? "Не указан" : city)
                        .font(InkFont.body)
                        .foregroundStyle(city.isEmpty ? Ink.secondary : Ink.label)
                        .inkRow()
                }
                InkGroupHeader(title: "Факты из разговоров")
                if facts.isEmpty {
                    InkEmpty(text: "Пока ничего")
                } else {
                    InkGroup {
                        ForEach(Array(facts.enumerated()), id: \.offset) { index, entry in
                            if index > 0 { InkDivider(inset: 16) }
                            Text(entry.text)
                                .font(InkFont.callout)
                                .foregroundStyle(Ink.label)
                                .fixedSize(horizontal: false, vertical: true)
                                .inkRow()
                        }
                    }
                }
                InkFootnote("Другие Selfvoy видят только, свободны ли вы в предложенное время, а не ваш календарь и не эти записи.")
                    .padding(.horizontal, 16)
            }
            .padding(20)
        }
        .inkScreen()
    }
}
