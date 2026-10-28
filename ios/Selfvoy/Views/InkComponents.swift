// InkComponents.swift — общие элементы дизайна 2026: статус (значок + слово),
// бейдж тарифа, аватар с инициалами, заголовки секций, ячейки сгруппированных
// списков, кнопки, поля, чипы, знак Selfvoy и шапка экрана с крупным заголовком.
import SelfvoyKit
import SwiftUI
import UIKit

/// Статус — всегда значок и слово, не только цвет.
struct StatusBadge: View {
    let status: StatusView
    var compact = false

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: Ink.symbol(status.icon)).font(.system(size: compact ? 11 : 12, weight: .semibold))
            Text(status.label).font(.system(size: compact ? 12 : 13, weight: .semibold)).lineLimit(1)
        }
        .foregroundStyle(Ink.tone(status.tone))
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Capsule().fill(status.tone == .attention ? Ink.amber.opacity(0.18) : Ink.fill.opacity(0.6)))
        .accessibilityElement(children: .combine)
    }
}

/// Бейдж тарифа: «Pro до 12 ноября» / «Free».
struct PlanBadge: View {
    let plan: SelfvoyPlan?

    var body: some View {
        if let plan {
            let pro = plan.isPro()
            HStack(spacing: 4) {
                Image(systemName: pro ? "star.fill" : "star").font(.system(size: 11, weight: .bold))
                Text(plan.badge()).font(.system(size: 12, weight: .bold))
            }
            .foregroundStyle(pro ? Color.white : Ink.secondary)
            .padding(.horizontal, 9)
            .padding(.vertical, 4)
            .background(Capsule().fill(pro ? Ink.accent : Ink.fill))
            .accessibilityLabel("Тариф: \(plan.badge())")
        }
    }
}

/// Аватар с инициалами на мягком фоне (как в списке клиентов макета).
struct InkAvatar: View {
    let name: String
    var size: CGFloat = 40

    private var initials: String {
        let parts = name.split(whereSeparator: { $0.isWhitespace })
        switch parts.count {
        case 0: return "?"
        case 1: return String(parts[0].prefix(1)).uppercased()
        default: return (String(parts[0].prefix(1)) + String(parts[1].prefix(1))).uppercased()
        }
    }

    private var tint: Color {
        let value = name.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) % 5 }
        return Ink.capsule(abs(value) + 1)
    }

    var body: some View {
        Text(initials)
            .font(.system(size: size * 0.36, weight: .bold))
            .foregroundStyle(tint)
            .frame(width: size, height: size)
            .background(Circle().fill(tint.opacity(0.16)))
            .accessibilityHidden(true)
    }
}

/// Шапка экрана: строка-эпиграф («ЧЕТВЕРГ, 22 ОКТЯБРЯ»), крупный заголовок, действия справа.
struct InkHeader<Trailing: View>: View {
    var eyebrow = ""
    let title: String
    @ViewBuilder var trailing: () -> Trailing

    init(eyebrow: String = "", title: String, @ViewBuilder trailing: @escaping () -> Trailing = { EmptyView() }) {
        self.eyebrow = eyebrow
        self.title = title
        self.trailing = trailing
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(alignment: .center) {
                Text(eyebrow.uppercased())
                    .font(InkFont.eyebrow)
                    .foregroundStyle(Ink.secondary)
                Spacer(minLength: 8)
                HStack(spacing: 8) { trailing() }
            }
            .frame(minHeight: 40)
            Text(title)
                .font(InkFont.largeTitle)
                .foregroundStyle(Ink.label)
                .accessibilityAddTraits(.isHeader)
        }
    }
}

/// Круглая стеклянная кнопка шапки (поиск, «+», колокольчик).
struct InkRoundButton: View {
    let systemImage: String
    let label: String
    var badge = 0
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            ZStack(alignment: .topTrailing) {
                Image(systemName: systemImage)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(Ink.label)
                    .frame(width: 38, height: 38)
                    .background(Circle().fill(Ink.card))
                if badge > 0 {
                    Text(badge > 99 ? "99+" : "\(badge)")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(.black)
                        .padding(.horizontal, 4)
                        .frame(minWidth: 16, minHeight: 16)
                        .background(Capsule().fill(Ink.amber))
                        .offset(x: 4, y: -2)
                }
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(badge > 0 ? "\(label): \(badge)" : label)
    }
}

/// Заголовок секции сгруппированного списка («ОПЛАТА И ОТМЕНА»).
struct InkSectionHeader: View {
    let title: String
    var trailing = ""

    var body: some View {
        HStack {
            Text(title).font(InkFont.title).foregroundStyle(Ink.label)
            Spacer()
            if !trailing.isEmpty {
                Text(trailing).font(InkFont.footnote).foregroundStyle(Ink.secondary)
            }
        }
        .padding(.top, 8)
    }
}

/// Ячейка-ссылка: значок в цветном квадрате, подпись, значение, шеврон.
struct InkCell: View {
    let symbol: String
    let tint: Color
    let title: String
    var value = ""
    var badge = 0
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 30, height: 30)
                    .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(tint))
                Text(title).font(InkFont.body).foregroundStyle(Ink.label)
                Spacer(minLength: 8)
                if badge > 0 {
                    Text("\(badge)").font(.system(size: 13, weight: .bold)).foregroundStyle(.black)
                        .padding(.horizontal, 7).padding(.vertical, 2)
                        .background(Capsule().fill(Ink.amber))
                }
                if !value.isEmpty {
                    Text(value).font(InkFont.callout).foregroundStyle(Ink.secondary).lineLimit(1)
                }
                Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Ink.tertiary)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 11)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// Группа ячеек на одной карточке с разделителями.
struct InkGroup<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        VStack(spacing: 0) { content() }
            .background(RoundedRectangle(cornerRadius: 18, style: .continuous).fill(Ink.card))
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

struct InkDivider: View {
    var inset: CGFloat = 58

    var body: some View {
        Rectangle().fill(Ink.separator).frame(height: 0.5).padding(.leading, inset)
    }
}

/// Основная кнопка: акцентная капсула во всю ширину.
struct InkPrimaryButtonStyle: ButtonStyle {
    var tint: Color = Ink.accent

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .semibold))
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, minHeight: 50)
            .background(Capsule().fill(tint))
            .opacity(configuration.isPressed ? 0.8 : 1)
    }
}

/// Вторичная кнопка: мягкая заливка.
struct InkSecondaryButtonStyle: ButtonStyle {
    var tint: Color = Ink.accent

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(tint)
            .padding(.horizontal, 14)
            .frame(minHeight: 40)
            .background(Capsule().fill(tint.opacity(0.12)))
            .opacity(configuration.isPressed ? 0.7 : 1)
    }
}

/// Плашка ошибки внутри экрана/окна (pro_required — с подсказкой про приглашение).
struct InkNotice: View {
    let text: String
    var tone: StatusTone = .red

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: tone == .red ? "exclamationmark.triangle.fill" : "info.circle.fill")
            Text(text).font(InkFont.footnote).frame(maxWidth: .infinity, alignment: .leading)
        }
        .foregroundStyle(Ink.tone(tone))
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Ink.tone(tone).opacity(0.12)))
    }
}

/// Пустое состояние.
struct InkEmpty: View {
    let text: String

    var body: some View {
        Text(text)
            .font(InkFont.callout)
            .foregroundStyle(Ink.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .inkCard()
    }
}

/// Тихая кнопка: акцентный текст без подложки («Отмена», «Забыли пароль?»).
struct InkQuietButtonStyle: ButtonStyle {
    var tint: Color = Ink.accent

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(tint)
            .frame(minHeight: 40)
            .contentShape(Rectangle())
            .opacity(configuration.isPressed ? 0.6 : 1)
    }
}

/// Кнопка с подписью, значком и «занято»: основная, вторичная, опасная или тихая.
struct InkButton: View {
    enum Kind {
        case primary
        case secondary
        case destructive
        case quiet
    }

    let title: String
    var systemImage: String?
    var kind: Kind = .primary
    var busy = false
    var enabled = true
    var fullWidth = true
    let action: () -> Void

    var body: some View {
        Group {
            switch kind {
            case .primary:
                Button(action: action) { label }.buttonStyle(InkPrimaryButtonStyle())
            case .secondary:
                Button(action: action) { label }.buttonStyle(InkSecondaryButtonStyle())
            case .destructive:
                Button(action: action) { label }.buttonStyle(InkSecondaryButtonStyle(tint: Ink.red))
            case .quiet:
                Button(action: action) { label }.buttonStyle(InkQuietButtonStyle())
            }
        }
        .disabled(!enabled || busy)
        .opacity(enabled ? 1 : 0.45)
    }

    private var label: some View {
        HStack(spacing: 8) {
            if busy {
                ProgressView()
                    .controlSize(.small)
                    .tint(kind == .primary ? Color.white : Ink.accent)
            } else if let systemImage {
                Image(systemName: systemImage)
            }
            if !title.isEmpty {
                Text(title).lineLimit(1)
            }
        }
        .frame(maxWidth: fullWidth ? .infinity : nil)
    }
}

/// Значок раздела в цветном квадрате (как в ячейках «Ещё»).
struct InkIconTile: View {
    let symbol: String
    let tint: Color
    var size: CGFloat = 34

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.47, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(RoundedRectangle(cornerRadius: size * 0.26, style: .continuous).fill(tint))
            .accessibilityHidden(true)
    }
}

/// Подпись группы сгруппированного списка: мелко, прописными, над карточкой.
struct InkGroupHeader: View {
    let title: String

    var body: some View {
        Text(title.uppercased())
            .font(InkFont.footnote)
            .foregroundStyle(Ink.secondary)
            .padding(.horizontal, 16)
            .padding(.top, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityAddTraits(.isHeader)
    }
}

/// Пояснение под группой или полем.
struct InkFootnote: View {
    let text: String
    var alignment: TextAlignment = .leading

    init(_ text: String, alignment: TextAlignment = .leading) {
        self.text = text
        self.alignment = alignment
    }

    var body: some View {
        Text(text)
            .font(InkFont.footnote)
            .foregroundStyle(Ink.secondary)
            .multilineTextAlignment(alignment)
            .fixedSize(horizontal: false, vertical: true)
    }
}

extension View {
    /// Отступы строки внутри InkGroup.
    func inkRow(vertical: CGFloat = 11) -> some View {
        padding(.horizontal, 16)
            .padding(.vertical, vertical)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Поле ввода 2026: подпись над полем, сплошная подложка (на фоне экрана — карточка,
/// внутри карточки — мягкая заливка), рамка-акцент в фокусе и значок справа.
struct InkField: View {
    @Binding var text: String
    let placeholder: String
    var label = ""
    var systemImage: String?
    var trailingLabel = ""
    var onTrailing: (() -> Void)?
    var secure = false
    var inCard = false
    var keyboard: UIKeyboardType = .default
    var submitLabel: SubmitLabel = .done
    var onSubmit: (() -> Void)?

    @FocusState private var focused: Bool

    init(text: Binding<String>, placeholder: String, label: String = "", systemImage: String? = nil,
         trailingLabel: String = "", onTrailing: (() -> Void)? = nil, secure: Bool = false,
         inCard: Bool = false, keyboard: UIKeyboardType = .default, submitLabel: SubmitLabel = .done,
         onSubmit: (() -> Void)? = nil) {
        _text = text
        self.placeholder = placeholder
        self.label = label
        self.systemImage = systemImage
        self.trailingLabel = trailingLabel
        self.onTrailing = onTrailing
        self.secure = secure
        self.inCard = inCard
        self.keyboard = keyboard
        self.submitLabel = submitLabel
        self.onSubmit = onSubmit
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if !label.isEmpty {
                Text(label).font(InkFont.eyebrow).foregroundStyle(Ink.secondary)
            }
            HStack(spacing: 10) {
                Group {
                    if secure {
                        SecureField(placeholder, text: $text)
                    } else {
                        TextField(placeholder, text: $text)
                    }
                }
                .font(InkFont.body)
                .foregroundStyle(Ink.label)
                .keyboardType(keyboard)
                .textInputAutocapitalization(keyboard == .emailAddress || keyboard == .URL || secure ? .never : .sentences)
                .autocorrectionDisabled(keyboard == .emailAddress || keyboard == .URL || secure)
                .submitLabel(submitLabel)
                .focused($focused)
                .onSubmit { onSubmit?() }
                if let systemImage {
                    if let onTrailing {
                        Button(action: onTrailing) {
                            Image(systemName: systemImage)
                                .font(.system(size: 16))
                                .foregroundStyle(Ink.secondary)
                                .frame(width: 32, height: 32)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(trailingLabel)
                    } else {
                        Image(systemName: systemImage)
                            .font(.system(size: 16))
                            .foregroundStyle(Ink.tertiary)
                            .accessibilityHidden(true)
                    }
                }
            }
            .padding(.leading, 14)
            .padding(.trailing, 10)
            .frame(minHeight: 48)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(inCard ? Ink.fill : Ink.card))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(focused ? Ink.accent : Color.clear, lineWidth: 1.5))
        }
    }
}

/// Чип-вариант: выбранный — акцентная заливка и галочка (не только цвет).
struct InkChip: View {
    let title: String
    var active = false
    var enabled = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if active {
                    Image(systemName: "checkmark").font(.system(size: 11, weight: .bold))
                }
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
            }
            .foregroundStyle(active ? Color.white : (enabled ? Ink.label : Ink.secondary))
            .padding(.horizontal, 13)
            .padding(.vertical, 7)
            .frame(minHeight: 34)
            .background(RoundedRectangle(cornerRadius: 17, style: .continuous).fill(active ? Ink.accent : Ink.fill))
            .contentShape(RoundedRectangle(cornerRadius: 17, style: .continuous))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

/// Знак Selfvoy: S-линия между двумя точками (как в макете selfvoy-app.html).
struct InkMark: View {
    var size: CGFloat = 40
    var line: Color = Ink.accent
    var dot: Color = Ink.label

    var body: some View {
        let k = size / 100
        ZStack(alignment: .topLeading) {
            InkMarkLine()
                .stroke(line, style: StrokeStyle(lineWidth: 11 * k, lineCap: .round))
            Circle().fill(dot)
                .frame(width: 20 * k, height: 20 * k)
                .offset(x: 66 * k, y: 18 * k)
            Circle().fill(Ink.amber)
                .frame(width: 20 * k, height: 20 * k)
                .offset(x: 14 * k, y: 62 * k)
        }
        .frame(width: size, height: size, alignment: .topLeading)
        .accessibilityHidden(true)
    }
}

/// Линия знака в координатах 100×100: M76 28 C38 24, 28 44, 50 50 C72 56, 62 76, 24 72.
private struct InkMarkLine: Shape {
    func path(in rect: CGRect) -> Path {
        let s = min(rect.width, rect.height) / 100
        var path = Path()
        path.move(to: CGPoint(x: 76 * s, y: 28 * s))
        path.addCurve(to: CGPoint(x: 50 * s, y: 50 * s),
                      control1: CGPoint(x: 38 * s, y: 24 * s), control2: CGPoint(x: 28 * s, y: 44 * s))
        path.addCurve(to: CGPoint(x: 24 * s, y: 72 * s),
                      control1: CGPoint(x: 72 * s, y: 56 * s), control2: CGPoint(x: 62 * s, y: 76 * s))
        return path
    }
}

/// Простая раскладка «в строку с переносом» для чипов (аналог FlowRow).
struct FlowLayout: Layout {
    var spacing: CGFloat = 8
    var alignment: HorizontalAlignment = .leading

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let rows = arrange(width: proposal.width ?? .infinity, subviews: subviews)
        let height = rows.reduce(0) { $0 + $1.height } + spacing * CGFloat(max(rows.count - 1, 0))
        let width = rows.map(\.width).max() ?? 0
        return CGSize(width: proposal.width ?? width, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for row in arrange(width: bounds.width, subviews: subviews) {
            var x = alignment == .trailing ? bounds.maxX - row.width : bounds.minX
            for index in row.items {
                let size = measure(subviews[index], maxWidth: bounds.width)
                subviews[index].place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
                x += size.width + spacing
            }
            y += row.height + spacing
        }
    }

    private struct Row {
        var items: [Int] = []
        var width: CGFloat = 0
        var height: CGFloat = 0
    }

    /// Элемент шире контейнера (длинная подсказка) ужимается до его ширины,
    /// иначе он вылезал за край экрана.
    private func measure(_ subview: LayoutSubview, maxWidth: CGFloat) -> CGSize {
        let ideal = subview.sizeThatFits(.unspecified)
        guard maxWidth.isFinite, ideal.width > maxWidth else { return ideal }
        return subview.sizeThatFits(ProposedViewSize(width: maxWidth, height: nil))
    }

    private func arrange(width: CGFloat, subviews: Subviews) -> [Row] {
        var rows: [Row] = [Row()]
        for index in subviews.indices {
            let size = measure(subviews[index], maxWidth: width)
            let extra = rows[rows.count - 1].items.isEmpty ? size.width : size.width + spacing
            if rows[rows.count - 1].width + extra > width, !rows[rows.count - 1].items.isEmpty {
                rows.append(Row())
            }
            var row = rows[rows.count - 1]
            row.width += row.items.isEmpty ? size.width : size.width + spacing
            row.height = max(row.height, size.height)
            row.items.append(index)
            rows[rows.count - 1] = row
        }
        return rows.filter { !$0.items.isEmpty }
    }
}
