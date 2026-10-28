pragma Singleton
import QtQuick
import QtCore

/*
  SelfvoyTheme — дизайн-система Selfvoy 2026 (как web/src/styles/global.css).

  Сплошные поверхности без стекла, свечения и градиентов под текстом; один
  акцент #3555F5, янтарный #FFB224 — «новое / внимание»; статус всегда
  значок + слово. Светлая и тёмная темы: по системе или выбором пользователя
  (mode: system | light | dark, хранится в настройках клиента).

  Старые имена токенов (bgDeep, glassFill, lilac, textPrimary…) оставлены
  синонимами новых, чтобы прежние экраны перекрасились без переписывания.
*/
QtObject {
    id: theme

    // --- тема: system | light | dark ----------------------------------------
    property Settings store: Settings {
        category: "appearance"
        property string mode: "system"
    }
    readonly property string mode: store.mode
    function setMode(value) {
        if (value === "light" || value === "dark" || value === "system") store.mode = value
    }
    readonly property bool systemDark: Qt.styleHints.colorScheme === Qt.Dark
    readonly property bool dark: mode === "dark" || (mode === "system" && systemDark)

    function pick(light, darkValue) { return theme.dark ? darkValue : light }

    // --- поверхности -----------------------------------------------------------
    readonly property color bg:        pick("#F2F2F7", "#000000")   // фон окна
    readonly property color bgElev:    pick("#FFFFFF", "#0F0F11")   // сайдбар
    readonly property color surface:   pick("#FFFFFF", "#1C1C1E")   // карточки, группы
    readonly property color surface2:  pick("#F7F7FA", "#232326")
    readonly property color surface3:  pick("#E5E5EA", "#3A3A3C")
    readonly property color field:     pick(Qt.rgba(118 / 255, 118 / 255, 128 / 255, 0.12), Qt.rgba(118 / 255, 118 / 255, 128 / 255, 0.24))
    readonly property color fieldHover: pick(Qt.rgba(118 / 255, 118 / 255, 128 / 255, 0.18), Qt.rgba(118 / 255, 118 / 255, 128 / 255, 0.32))
    readonly property color border:    pick(Qt.rgba(60 / 255, 60 / 255, 67 / 255, 0.13), Qt.rgba(84 / 255, 84 / 255, 88 / 255, 0.45))
    readonly property color borderStrong: pick(Qt.rgba(60 / 255, 60 / 255, 67 / 255, 0.24), Qt.rgba(84 / 255, 84 / 255, 88 / 255, 0.75))
    readonly property color ter:       pick("#C4C4CA", "#48484A")   // пунктир ленты, пустые кружки
    readonly property color dim:       pick(Qt.rgba(0, 0, 0, 0.32), Qt.rgba(0, 0, 0, 0.55))

    // --- текст -------------------------------------------------------------------
    readonly property color text:   pick("#111114", "#F5F5F7")
    readonly property color text2:  pick("#6C6C74", "#98989F")
    readonly property color text3:  pick("#8A8A91", "#75757C")

    // --- акцент и статусы ----------------------------------------------------------
    readonly property color accent:      pick("#3555F5", "#7D93FF")   // текст ссылок, активный пункт
    readonly property color accentSolid: "#3555F5"                    // заливка кнопок и карточки «Следующая»
    readonly property color accentHover: pick("#2A47E0", "#4A67FF")
    readonly property color accentTint:  pick("#E9EDFF", Qt.rgba(125 / 255, 147 / 255, 1, 0.18))
    readonly property color amber:       "#FFB224"                    // новое / внимание (знак бренда)
    readonly property color newSoft:     pick("#FFF1D6", Qt.rgba(1, 178 / 255, 36 / 255, 0.18))
    readonly property color success:     pick("#1E8E50", "#4CD483")
    readonly property color successSoft: pick("#E3F5EA", Qt.rgba(76 / 255, 212 / 255, 131 / 255, 0.15))
    readonly property color warning:     pick("#A86400", "#FFC24D")
    readonly property color warningSoft: pick("#FFF1D6", Qt.rgba(1, 194 / 255, 77 / 255, 0.15))
    readonly property color danger:      pick("#D93A3F", "#FF6B6F")
    readonly property color dangerSoft:  pick("#FDECEC", Qt.rgba(1, 107 / 255, 111 / 255, 0.15))
    readonly property color toggleOn:    pick("#34C759", "#30D158")
    readonly property color online:      toggleOn

    // Цвета капсул ленты дня — по странице записи (5 по кругу).
    readonly property var capsule: ["#3555F5", "#22A06B", "#F59E0B", "#E8618C", "#7C5CFC"]
    // Карточка «Следующая» на тех же цветах, но темнее там, где белый текст иначе не читается.
    readonly property var capsuleDeep: ["#3555F5", "#22A06B", "#C27803", "#C94473", "#7C5CFC"]
    function capsuleColor(index) { return capsule[((index - 1) % 5 + 5) % 5] }
    function capsuleDeepColor(index) { return capsuleDeep[((index - 1) % 5 + 5) % 5] }

    // Тон статуса (как .status-pill в вебе): green | amber | new | red | accent | "".
    function toneFg(tone) {
        switch (tone) {
        case "green": return success
        case "amber": case "new": return warning
        case "red": return danger
        case "accent": return accent
        default: return text2
        }
    }
    function toneBg(tone) {
        switch (tone) {
        case "green": return successSoft
        case "amber": return warningSoft
        case "new": return newSoft
        case "red": return dangerSoft
        case "accent": return accentTint
        default: return field
        }
    }

    // --- синонимы прежней темы (экраны v3) ------------------------------------------
    readonly property color bgDeep: bg
    readonly property color bgBase: bg
    readonly property color bgRaised: surface
    readonly property color bgElevated: pick("#FFFFFF", "#232326")
    readonly property color bgPanel: surface
    readonly property real  glassFill: 0.06
    readonly property real  glassFillHover: 0.09
    readonly property real  glassFillPressed: 0.12
    readonly property real  glassBorder: dark ? 0.16 : 0.12
    readonly property real  glassBorderHover: dark ? 0.26 : 0.2
    readonly property real  glassHighlight: 0
    readonly property color glassTint: text
    readonly property color glassEdge: border
    readonly property color accentSoft: accent          // «акцентный текст» в старых экранах
    readonly property color accentDeep: accentHover
    readonly property real  accentFill: 0.16
    readonly property color accentGlow: "transparent"
    readonly property color gradStart: accentSolid
    readonly property color gradMid: accentSolid
    readonly property color gradEnd: accentSolid
    readonly property color glowViolet: bg
    readonly property color glowMagenta: bg
    readonly property color glowIndigo: bg
    readonly property color lilac: accent
    readonly property color pink: capsule[3]
    readonly property color textPrimary: text
    readonly property color textSecondary: text2
    readonly property color textMuted: text3
    readonly property color textOnAccent: "#FFFFFF"

    // --- геометрия (r-sm 10, r-md 12, r-lg 20, r-xl 28) -----------------------------
    readonly property int radiusPill:   999
    readonly property int radiusShell:  28
    readonly property int radiusSheet:  28
    readonly property int radiusCard:   20
    readonly property int radiusBubble: 20
    readonly property int radiusField:  12
    readonly property int radiusSmall:  10

    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 16
    readonly property int spaceLg: 24
    readonly property int spaceXl: 32
    readonly property int spaceXxl: 48

    // --- раскладка: сайдбар + контент ----------------------------------------------
    readonly property int breakpointWide: 1000
    readonly property int sidebarWidth: 248
    readonly property int railWidth: 76
    readonly property int masterWidth: 340

    // --- тень (только у всплывающих окон) ---------------------------------------------
    readonly property color shadowColor: pick(Qt.rgba(20 / 255, 24 / 255, 40 / 255, 0.22), Qt.rgba(0, 0, 0, 0.7))
    readonly property int   shadowBlur:  30
    readonly property int   shadowY:     10

    // --- типографика: системный шрифт, крупные жирные заголовки -------------------------
    readonly property string fontFamily: Qt.application.font.family
    readonly property int fontMicro:   12
    readonly property int fontSmall:   13
    readonly property int fontBody:    15
    readonly property int fontLead:    17
    readonly property int fontTitle:   20
    readonly property int fontHero:    34
    readonly property int fontDisplay: 40

    // --- анимация ---------------------------------------------------------------------
    readonly property int  pressMs:  150
    readonly property int  moveMs:   220
    readonly property int  slowMs:   320
    readonly property real pressScale: 0.98
    readonly property real hoverLift:  0
    readonly property int  ease: Easing.OutCubic

    // Полупрозрачные оттенки. glass() — «чернила» поверх поверхности: в светлой
    // теме тёмные, в тёмной — светлые (ховеры, разделители, подложки).
    function glass(alpha)   { return dark ? Qt.rgba(1, 1, 1, alpha) : Qt.rgba(0.07, 0.07, 0.1, alpha * 0.9) }
    function accentA(alpha) { return Qt.rgba(accent.r, accent.g, accent.b, alpha) }
    function lilacA(alpha)  { return accentA(alpha) }
    function shade(alpha)   { return Qt.rgba(0, 0, 0, alpha) }

    // Приветствие по времени суток
    function greeting(date) {
        const h = date.getHours()
        if (h < 5) return "Доброй ночи"
        if (h < 12) return "Доброе утро"
        if (h < 18) return "Добрый день"
        return "Добрый вечер"
    }
}
