// SlotsPostSheet.swift — «Пост со свободными окнами» (F6, booking.slotsPost):
// готовый текст с реальными окнами страницы для Telegram-канала, VK или
// куда угодно — скопировать или отправить через системное «Поделиться».
import SelfvoyKit
import SwiftUI
import UIKit

struct SlotsPostSheet: View {

    @EnvironmentObject private var store: AppStore
    @Environment(\.dismiss) private var dismiss
    @State var page: BookingPage

    @State private var format: SlotsPostFormat = .telegram
    @State private var days = 7
    @State private var post: SlotsPost?
    @State private var loading = false
    @State private var error = ""

    private var activePages: [BookingPage] { (store.bookingPages ?? []).filter(\.isActive) }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if activePages.count > 1 {
                        Picker("Страница", selection: Binding(get: { page.id }, set: { id in
                            if let found = activePages.first(where: { $0.id == id }) { page = found }
                        })) {
                            ForEach(activePages) { item in Text(item.fields.title).tag(item.id) }
                        }
                        .pickerStyle(.menu)
                    }
                    Picker("Куда", selection: $format) {
                        ForEach(SlotsPostFormat.allCases, id: \.self) { item in Text(item.title).tag(item) }
                    }
                    .pickerStyle(.segmented)
                    Picker("Дней", selection: $days) {
                        Text("3 дня").tag(3)
                        Text("Неделя").tag(7)
                        Text("2 недели").tag(14)
                    }
                    .pickerStyle(.segmented)
                    preview
                    if !error.isEmpty { InkNotice(text: error) }
                    if let post, !post.text.isEmpty {
                        let text = post.shareText(fallbackLink: page.url.isEmpty ? nil : page.url)
                        ShareLink(item: text) {
                            Label("Поделиться", systemImage: "square.and.arrow.up")
                        }
                        .buttonStyle(InkPrimaryButtonStyle())
                        Button {
                            UIPasteboard.general.string = text
                            store.setStatus("Текст скопирован — вставьте его в канал или чат")
                        } label: {
                            Label("Скопировать текст", systemImage: "doc.on.doc")
                                .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(InkSecondaryButtonStyle())
                    }
                }
                .padding(20)
            }
            .background(Ink.bg)
            .navigationTitle("Пост со свободными окнами")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Готово") { dismiss() } }
            }
        }
        .tint(Ink.accent)
        .task(id: "\(page.id)-\(format.rawValue)-\(days)") { await load() }
    }

    @ViewBuilder
    private var preview: some View {
        if loading && post == nil {
            HStack { Spacer(); ProgressView(); Spacer() }.inkCard()
        } else if let post {
            VStack(alignment: .leading, spacing: 8) {
                Text(post.slots > 0 ? "Окон в посте: \(post.slots)" : "Свободных окон нет")
                    .font(InkFont.eyebrow).foregroundStyle(post.slots > 0 ? Ink.secondary : Ink.amberInk)
                Text(post.text)
                    .font(InkFont.callout)
                    .foregroundStyle(Ink.label)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .inkCard()
            .opacity(loading ? 0.5 : 1)
        }
    }

    @MainActor
    private func load() async {
        loading = true
        error = ""
        do {
            post = try await store.slotsPost(page: page, days: days, format: format)
        } catch {
            self.error = store.describe(error)
        }
        loading = false
    }
}
