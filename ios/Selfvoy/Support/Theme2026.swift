// Theme2026.swift — визуальный язык Selfvoy 2026 (как web/src/styles и макет
// selfvoy-app.html): системный шрифт, крупные жирные заголовки, один акцент
// #3555F5, янтарь #FFB224 — «новое/внимание», светлая и тёмная темы, стекло —
// только на плавающих панелях и кнопках; текст всегда на сплошном фоне.
//
// На нём все экраны приложения; тема — системная (светлая или тёмная), цвета
// ниже сами меняются вместе с ней.
import SelfvoyKit
import SwiftUI
import UIKit

extension Color {
    /// Цвет из 0xRRGGBB.
    init(hex: UInt32, alpha: Double = 1) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: alpha)
    }

    /// Цвет, который сам меняется со светлой/тёмной темой.
    init(light: UInt32, dark: UInt32, lightAlpha: Double = 1, darkAlpha: Double = 1) {
        self.init(UIColor { traits in
            let dark_ = traits.userInterfaceStyle == .dark
            let hex = dark_ ? dark : light
            return UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255,
                           green: CGFloat((hex >> 8) & 0xFF) / 255,
                           blue: CGFloat(hex & 0xFF) / 255,
                           alpha: dark_ ? darkAlpha : lightAlpha)
        })
    }
}

enum Ink {
    static let bg = Color(light: 0xF2F2F7, dark: 0x000000)
    static let card = Color(light: 0xFFFFFF, dark: 0x1C1C1E)
    static let raised = Color(light: 0xFFFFFF, dark: 0x2C2C2E)
    static let fill = Color(light: 0x787880, dark: 0x787880, lightAlpha: 0.12, darkAlpha: 0.24)
    static let label = Color(light: 0x0B0B0F, dark: 0xFFFFFF)
    static let secondary = Color(light: 0x3C3C43, dark: 0xEBEBF5, lightAlpha: 0.62, darkAlpha: 0.62)
    static let tertiary = Color(light: 0x3C3C43, dark: 0xEBEBF5, lightAlpha: 0.32, darkAlpha: 0.32)
    static let separator = Color(light: 0x3C3C43, dark: 0x545458, lightAlpha: 0.18, darkAlpha: 0.5)

    /// Единственный акцент; в тёмной теме чуть светлее для контраста.
    static let accent = Color(light: 0x3555F5, dark: 0x5B76FF)
    static let accentSoft = Color(light: 0x3555F5, dark: 0x5B76FF, lightAlpha: 0.12, darkAlpha: 0.22)
    /// Фон карточки «Следующая встреча» — всегда #3555F5, текст белый.
    static let hero = Color(hex: 0x3555F5)
    static let amber = Color(hex: 0xFFB224)
    static let amberInk = Color(light: 0xA35F00, dark: 0xFFC53D)
    static let green = Color(light: 0x18794E, dark: 0x3DD68C)
    static let red = Color(light: 0xD93036, dark: 0xFF6369)
    static let violet = Color(light: 0x7C3AED, dark: 0xA78BFA)
    static let teal = Color(light: 0x0E7C86, dark: 0x2EC4CF)

    /// Цвет капсулы ленты дня (тип встречи = страница записи), 1...5.
    static func capsule(_ index: Int) -> Color {
        switch (index - 1) % 5 {
        case 0: return accent
        case 1: return violet
        case 2: return amber
        case 3: return green
        default: return teal
        }
    }

    static func tone(_ tone: StatusTone) -> Color {
        switch tone {
        case .green: return green
        case .amber, .attention: return amberInk
        case .red: return red
        case .accent: return accent
        case .neutral: return secondary
        }
    }

    static func symbol(_ icon: StatusIcon) -> String {
        switch icon {
        case .check: return "checkmark.circle.fill"
        case .clock: return "clock.fill"
        case .cross: return "xmark.circle.fill"
        case .undo: return "arrow.uturn.backward.circle.fill"
        case .minus: return "minus.circle.fill"
        case .hand: return "hand.raised.fill"
        case .plus: return "plus.circle.fill"
        case .bell: return "bell.fill"
        case .calendar: return "calendar"
        }
    }
}

/// Крупные жирные заголовки системным шрифтом.
enum InkFont {
    static let largeTitle = Font.system(size: 34, weight: .bold)
    static let title = Font.system(size: 22, weight: .bold)
    static let headline = Font.system(size: 17, weight: .semibold)
    static let body = Font.system(size: 17)
    static let callout = Font.system(size: 15)
    static let footnote = Font.system(size: 13)
    static let caption = Font.system(size: 12, weight: .medium)
    static let eyebrow = Font.system(size: 13, weight: .semibold)
    static let heroTime = Font.system(size: 48, weight: .bold).monospacedDigit()
}

// MARK: - Стекло

/// Плавающая стеклянная поверхность: на iOS 26 — системное Liquid Glass
/// (`glassEffect`), раньше — `.ultraThinMaterial` с тонким кантом.
struct FloatingGlass<S: Shape>: ViewModifier {
    let shape: S

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.glassEffect(.regular.interactive(), in: shape)
        } else {
            fallback(content)
        }
        #else
        fallback(content)
        #endif
    }

    private func fallback(_ content: Content) -> some View {
        content
            .background(shape.fill(.ultraThinMaterial))
            .overlay(shape.stroke(Color.white.opacity(0.35), lineWidth: 0.5))
            .shadow(color: .black.opacity(0.14), radius: 16, y: 6)
    }
}

extension View {
    func floatingGlass<S: Shape>(_ shape: S) -> some View {
        modifier(FloatingGlass(shape: shape))
    }

    /// Карточка на сплошном фоне (текст всегда читается).
    func inkCard(padding: CGFloat = 16, radius: CGFloat = 22) -> some View {
        self.padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: radius, style: .continuous).fill(Ink.card))
    }

    /// Фон экранов 2026.
    func inkScreen() -> some View {
        background(Ink.bg.ignoresSafeArea())
    }

    /// Окно снизу: крупные скругления, «ручка», средняя и полная высота.
    func inkSheet(large: Bool = false) -> some View {
        presentationDetents(large ? [.large] : [.medium, .large])
            .presentationDragIndicator(.visible)
            .presentationCornerRadius(32)
    }
}
