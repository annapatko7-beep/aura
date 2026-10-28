// ConfirmationsView.swift — опасные действия Selfvoy, ждущие решения человека
// (confirmation.approve / confirmation.deny), в дизайне 2026. Сам Selfvoy их не выполняет.
import SelfvoyKit
import SwiftUI

struct ConfirmationsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        VStack(spacing: 0) {
            SubscreenBar()
            list
        }
        .inkScreen()
    }

    private var list: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: "Ждут решения", title: "Подтверждения")
                Text("Письма, сообщения и брони Selfvoy не выполняет сам: каждое такое действие ждёт здесь вашего решения.")
                    .font(InkFont.callout)
                    .foregroundStyle(Ink.secondary)
                if store.confirmations.isEmpty {
                    InkEmpty(text: "Сейчас ничего не ждёт решения.")
                }
                ForEach(store.confirmations) { action in
                    VStack(alignment: .leading, spacing: 10) {
                        StatusBadge(status: StatusView("Требуется подтверждение", .attention, .hand), compact: true)
                        Text(action.summary.isEmpty ? action.tool : action.summary)
                            .font(InkFont.headline)
                            .foregroundStyle(Ink.label)
                        Text("Инструмент: \(Format.toolLabel(action.tool))")
                            .font(InkFont.footnote)
                            .foregroundStyle(Ink.secondary)
                        HStack(spacing: 10) {
                            InkButton(title: "Подтвердить", systemImage: "checkmark", kind: .secondary) {
                                store.decideConfirmation(id: action.id, approve: true)
                            }
                            InkButton(title: "Отклонить", systemImage: "xmark", kind: .destructive) {
                                store.decideConfirmation(id: action.id, approve: false)
                            }
                        }
                        .padding(.top, 4)
                    }
                    .inkCard()
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .refreshable { await store.loadConfirmations() }
    }
}
