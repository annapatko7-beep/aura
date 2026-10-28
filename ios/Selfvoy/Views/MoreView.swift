// MoreView.swift — «Ещё» (дизайн 2026): профиль с бейджем тарифа, награда за
// приглашение коллеги (месяц Pro обоим, F3), абонементы, подключения, входящие и
// остальные разделы.
import SelfvoyKit
import SwiftUI
import UIKit

struct MoreView: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                InkHeader(title: "Ещё")
                profile
                ReferralRewardCard()
                InkGroup {
                    InkCell(symbol: "ticket.fill", tint: Ink.violet, title: "Абонементы",
                            value: activePackages) { store.selectTab(.packages) }
                    InkDivider()
                    InkCell(symbol: "link", tint: Ink.accent, title: "Подключения",
                            value: integrationsSummary) { store.selectTab(.integrations) }
                    InkDivider()
                    InkCell(symbol: "megaphone.fill", tint: Ink.amber, title: "Пост со свободными окнами") {
                        store.bookingIntent = .post
                        store.selectTab(.booking)
                    }
                }
                InkGroup {
                    InkCell(symbol: "bell.fill", tint: Ink.red, title: "Уведомления",
                            badge: Int(store.unreadNotifications)) { store.selectTab(.notifications) }
                    InkDivider()
                    InkCell(symbol: "exclamationmark.shield.fill", tint: Ink.amber, title: "Ждут решения",
                            badge: store.confirmations.count) { store.selectTab(.confirmations) }
                }
                InkGroup {
                    InkCell(symbol: "person.2.fill", tint: Ink.green, title: "Переговоры") { store.selectTab(.negotiations) }
                    InkDivider()
                    InkCell(symbol: "checkmark.square.fill", tint: Ink.teal, title: "Задачи") { store.selectTab(.tasks) }
                    InkDivider()
                    InkCell(symbol: "bubble.left.and.bubble.right.fill", tint: Ink.accent, title: "Контакты и чаты") {
                        store.selectTab(.contacts)
                    }
                }
                InkGroup {
                    InkCell(symbol: "gearshape.fill", tint: Color(hex: 0x8E8E93), title: "Настройки") {
                        store.selectTab(.settings)
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
        .inkScreen()
        .onAppear {
            if store.packages == nil { store.loadPackages() }
            if store.telegram == nil { store.loadGrowthIntegrations() }
        }
    }

    private var profile: some View {
        Button {
            store.selectTab(.settings)
        } label: {
            HStack(spacing: 12) {
                InkAvatar(name: store.userName, size: 52)
                VStack(alignment: .leading, spacing: 4) {
                    Text(store.userName).font(InkFont.headline).foregroundStyle(Ink.label)
                    HStack(spacing: 6) {
                        PlanBadge(plan: store.userPlan)
                        Text(store.userEmail).font(InkFont.footnote).foregroundStyle(Ink.secondary).lineLimit(1)
                    }
                    if let plan = store.userPlan {
                        Text(plan.hint()).font(InkFont.caption).foregroundStyle(Ink.secondary)
                    }
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right").foregroundStyle(Ink.tertiary)
            }
            .inkCard()
        }
        .buttonStyle(.plain)
    }

    private var activePackages: String {
        guard let list = store.packages else { return "" }
        let count = list.filter { $0.status == "active" }.count
        return count > 0 ? "\(count)" : ""
    }

    private var integrationsSummary: String {
        var parts: [String] = []
        if store.telegram?.linked == true { parts.append("Telegram") }
        if store.payments?.connected == true { parts.append("ЮKassa") }
        if store.calendarFeed?.enabled == true { parts.append("Календарь") }
        return parts.joined(separator: ", ")
    }
}

/// «Пригласите коллегу — месяц Pro вам обоим»: код/ссылка, счётчик наград.
struct ReferralRewardCard: View {

    @EnvironmentObject private var store: AppStore

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Image(systemName: "gift.fill")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 34, height: 34)
                    .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(Ink.amber))
                VStack(alignment: .leading, spacing: 2) {
                    Text("Пригласите коллегу").font(InkFont.headline).foregroundStyle(Ink.label)
                    Text("Месяц Pro — вам и ему, за каждого, кто заведёт Selfvoy")
                        .font(InkFont.footnote).foregroundStyle(Ink.secondary)
                }
            }
            if let info = store.referral {
                Text(info.rewardText).font(InkFont.footnote).foregroundStyle(Ink.secondary)
                if info.rewardCap > 0 {
                    ProgressView(value: Double(min(info.rewarded, info.rewardCap)), total: Double(info.rewardCap))
                        .tint(Ink.amber)
                }
                HStack(spacing: 8) {
                    ShareLink(item: info.shareText) {
                        Label("Пригласить", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(InkSecondaryButtonStyle())
                    Button {
                        UIPasteboard.general.string = info.link
                        store.setStatus(info.hasLink ? "Ссылка скопирована" : "Код скопирован")
                    } label: {
                        Label(info.hasLink ? "Ссылка" : info.code, systemImage: "doc.on.doc")
                    }
                    .buttonStyle(InkSecondaryButtonStyle(tint: Ink.secondary))
                }
                Text("Зарегистрировались: \(info.joined) · подтвердили email: \(info.verified)")
                    .font(InkFont.caption).foregroundStyle(Ink.secondary)
            } else {
                Text("Загружаем код приглашения…").font(InkFont.footnote).foregroundStyle(Ink.secondary)
            }
        }
        .inkCard()
        .onAppear { if store.referral == nil { store.loadReferral() } }
    }
}
