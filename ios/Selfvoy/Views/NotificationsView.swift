// NotificationsView.swift — входящие уведомления (дизайн 2026, notifications.list/read)
// и устройства, на которые приходят push (devices.push.*).
import SelfvoyKit
import SwiftUI

struct NotificationsView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        VStack(spacing: 0) {
            // «Прочитать все» — в строке «назад»: она закреплена над прокруткой.
            SubscreenBar {
                if store.notices.contains(where: { !$0.read }) {
                    Button("Прочитать все") { store.markNotificationsRead() }
                        .buttonStyle(InkSecondaryButtonStyle())
                }
            }
            list
        }
        .inkScreen()
        .onAppear { store.loadPushDevices() }
    }

    private var list: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                InkHeader(eyebrow: unreadText, title: "Уведомления")
                if store.notices.isEmpty {
                    InkEmpty(text: "Пока пусто.")
                } else {
                    InkGroup {
                        ForEach(Array(store.notices.enumerated()), id: \.element.id) { index, notice in
                            if index > 0 { InkDivider(inset: 58) }
                            row(notice)
                        }
                    }
                }
                InkSectionHeader(title: "Push-устройства")
                if store.pushDevices.isEmpty {
                    InkEmpty(text: "Push на этом устройстве ещё не зарегистрирован: разрешите уведомления в настройках iOS.")
                } else {
                    InkGroup {
                        ForEach(Array(store.pushDevices.enumerated()), id: \.element.id) { index, device in
                            if index > 0 { InkDivider(inset: 16) }
                            HStack(spacing: 10) {
                                Image(systemName: device.enabled ? "checkmark.circle.fill" : "minus.circle.fill")
                                    .foregroundStyle(device.enabled ? Ink.green : Ink.secondary)
                                    .accessibilityHidden(true)
                                Text("\(device.platform.uppercased()) · \(device.enabled ? "включено" : "выключено")")
                                    .font(InkFont.callout)
                                    .foregroundStyle(Ink.label)
                                Spacer(minLength: 8)
                                Button("Отозвать") { store.revokePushDevice(id: device.id) }
                                    .buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
                            }
                            .inkRow(vertical: 8)
                        }
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 4)
            .padding(.bottom, 24)
        }
        .refreshable { store.loadNotifications() }
    }

    private var unreadText: String {
        let count = store.notices.filter { !$0.read }.count
        return count > 0 ? "Новых: \(count)" : "Всё прочитано"
    }

    private func row(_ notice: Notice) -> some View {
        Button {
            if !notice.read { store.markNotificationsRead(id: notice.id) }
            open(notice)
        } label: {
            HStack(alignment: .top, spacing: 12) {
                let style = Self.style(notice.kind)
                InkIconTile(symbol: style.symbol, tint: style.tint, size: 30)
                    .opacity(notice.read ? 0.55 : 1)
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .firstTextBaseline, spacing: 6) {
                        Text(notice.title)
                            .font(notice.read ? InkFont.body : InkFont.headline)
                            .foregroundStyle(Ink.label)
                        Spacer(minLength: 4)
                        if !notice.read {
                            Circle().fill(Ink.accent).frame(width: 8, height: 8)
                                .accessibilityHidden(true)
                        }
                    }
                    // Записи, оплаты, пакеты, награды — статус значком и словом (дизайн 2026).
                    if Self.growthKinds.contains(where: { notice.kind.hasPrefix($0) }) {
                        StatusBadge(status: SelfvoyNotificationKind.status(notice.kind, payload: notice.payload),
                                    compact: true)
                    }
                    // У login.new в body сетевой адрес — пользователю он ни о чём не говорит.
                    if !notice.body.isEmpty && notice.kind != "login.new" {
                        Text(notice.body).font(InkFont.callout).foregroundStyle(Ink.secondary)
                    }
                    Text("\(Format.dayPhrase(notice.createdAt)), \(Format.timeOf(notice.createdAt))")
                        .font(InkFont.caption)
                        .foregroundStyle(Ink.secondary)
                }
            }
            .multilineTextAlignment(.leading)
            .inkRow()
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityValue(notice.read ? "" : "Не прочитано")
    }

    /// Значок и цвет квадрата по виду уведомления.
    private struct KindStyle {
        let symbol: String
        let tint: Color
    }

    private static func style(_ kind: String) -> KindStyle {
        if kind.hasPrefix("twofactor") || kind == "login.new" { return KindStyle(symbol: "lock.shield.fill", tint: Ink.amber) }
        if kind == "confirmation.requested" { return KindStyle(symbol: "exclamationmark.shield.fill", tint: Ink.red) }
        if kind == "task.due" { return KindStyle(symbol: "checkmark.square.fill", tint: Ink.teal) }
        if kind.hasPrefix("booking.") { return KindStyle(symbol: "calendar", tint: Ink.accent) }
        if kind.hasPrefix("package.") { return KindStyle(symbol: "ticket.fill", tint: Ink.violet) }
        if kind.hasPrefix("referral.") { return KindStyle(symbol: "gift.fill", tint: Ink.amber) }
        if kind.hasPrefix("chat.") { return KindStyle(symbol: "bubble.left.fill", tint: Ink.green) }
        return KindStyle(symbol: "bell.fill", tint: Ink.accent)
    }

    private static let growthKinds = ["booking.", "package.", "referral."]

    /// Уведомление ведёт в свой раздел: записи, абонементы, тариф.
    private func open(_ notice: Notice) {
        if notice.kind.hasPrefix("booking.") {
            store.selectTab(.booking)
        } else if notice.kind.hasPrefix("package.") {
            store.selectTab(.packages)
        } else if notice.kind.hasPrefix("referral.") {
            store.selectTab(.more)
        }
    }
}
