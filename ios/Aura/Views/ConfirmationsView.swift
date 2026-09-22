// ConfirmationsView.swift — барьер подтверждения опасных операций (этап 8).
import SwiftUI

struct ConfirmationsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        NavigationStack {
            List {
                if store.confirmations.isEmpty {
                    Text("Нет операций, ожидающих подтверждения")
                        .foregroundStyle(.secondary)
                }
                ForEach(store.confirmations.indices, id: \.self) { index in
                    let action = store.confirmations[index]
                    VStack(alignment: .leading, spacing: 6) {
                        Text(action.string("tool"))
                            .font(.headline)
                        Text(action.string("summary"))
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        Text("истекает: \(action.string("expires_at"))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        HStack(spacing: 16) {
                            Button("Разрешить") {
                                store.decideConfirmation(id: action.int("id"), approve: true)
                            }
                            .buttonStyle(.borderedProminent)
                            Button("Отклонить", role: .destructive) {
                                store.decideConfirmation(id: action.int("id"), approve: false)
                            }
                            .buttonStyle(.bordered)
                        }
                        .font(.footnote)
                    }
                    .padding(.vertical, 4)
                }
            }
            .navigationTitle("Подтверждения")
            .refreshable { store.loadConfirmations() }
        }
    }
}
