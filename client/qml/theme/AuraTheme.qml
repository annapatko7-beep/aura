pragma Singleton
import QtQuick

/*
  AuraTheme — дизайн-система «Dark Glassmorphism» (v2).

  Тёмный графит + полупрозрачные пузыри в духе Apple: тонкая светлая рамка,
  верхний блик, мягкая тень и размытие фона. Акцент — мягкий фиолет #7C6AFF.
  Значения один в один с preview/styles.css и docs/DESIGN.md.
*/
QtObject {
    // --- фон ------------------------------------------------------------
    readonly property color bgDeep:      "#0D0D0F"
    readonly property color bgBase:      "#16161A"
    readonly property color bgRaised:    "#1E1E24"   // карточки, секции
    readonly property color bgElevated:  "#26262E"   // модалки, дропдауны

    // --- стекло ---------------------------------------------------------
    readonly property real  glassFill:        0.04
    readonly property real  glassFillHover:   0.08
    readonly property real  glassFillPressed: 0.12
    readonly property real  glassBorder:      0.08
    readonly property real  glassBorderHover: 0.15
    readonly property real  glassHighlight:   0.14   // верхний блик
    readonly property color glassTint:        "#FFFFFF"

    // --- акцент ---------------------------------------------------------
    readonly property color accent:       "#7C6AFF"
    readonly property color accentSoft:   "#A78BFA"  // сообщения Ауры, подсветка
    readonly property color accentDeep:   "#5B4DD6"
    readonly property real  accentFill:   0.20
    readonly property color accentGlow:   "#407C6AFF" // свечение акцентных кнопок

    // --- текст ----------------------------------------------------------
    readonly property color textPrimary:   "#F0F0F4"
    readonly property color textSecondary: "#9A9AA4"
    readonly property color textMuted:     "#5C5C66"
    readonly property color textOnAccent:  "#FFFFFF"

    // --- состояния ------------------------------------------------------
    readonly property color success: "#4ADE80"
    readonly property color warning: "#FBBF24"
    readonly property color danger:  "#F87171"
    readonly property color online:  "#4ADE80"

    // --- геометрия ------------------------------------------------------
    readonly property int radiusPill:   999
    readonly property int radiusCard:   22
    readonly property int radiusBubble: 22
    readonly property int radiusField:  14
    readonly property int radiusSmall:  14

    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 16
    readonly property int spaceLg: 24
    readonly property int spaceXl: 32

    // --- тень (0 8px 32px rgba(0,0,0,0.35)) -----------------------------
    readonly property color shadowColor: "#59000000"
    readonly property int   shadowBlur:  32
    readonly property int   shadowY:     8

    // --- типографика ----------------------------------------------------
    readonly property string fontFamily: "Inter, 'SF Pro Text', 'Segoe UI', 'Noto Sans', sans-serif"
    readonly property int fontMicro:  11
    readonly property int fontSmall:  12
    readonly property int fontBody:   14
    readonly property int fontTitle:  18
    readonly property int fontHero:   28

    // --- анимация: fade + slide-up, 200–300 мс ---------------------------
    readonly property int  pressMs:  200
    readonly property int  moveMs:   250
    readonly property int  slowMs:   300
    readonly property real pressScale: 0.97
    readonly property real hoverLift:  -1     // translateY(-1px) при наведении
    readonly property int  ease: Easing.OutCubic // ≈ cubic-bezier(0.25,0.46,0.45,0.94)

    // Вспомогательные цвета с прозрачностью
    function glass(alpha)      { return Qt.rgba(1, 1, 1, alpha) }
    function accentA(alpha)    { return Qt.rgba(accent.r, accent.g, accent.b, alpha) }
    function shade(alpha)      { return Qt.rgba(0, 0, 0, alpha) }
}
