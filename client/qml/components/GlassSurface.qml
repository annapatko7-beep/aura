import QtQuick
import Selfvoy

/*
  GlassSurface — базовая поверхность 2026: сплошная заливка (surface) с тонкой
  рамкой. Имя и свойства (backdrop, fill, edge, base…) сохранены для старых
  экранов; размытия и бликов больше нет — текст лежит на сплошном фоне.
*/
Item {
    id: root

    property Item backdrop: null          // не используется: без размытия
    property real blurStrength: 0
    property color tint: SelfvoyTheme.surface
    property real fill: SelfvoyTheme.glassFill
    property real fillHover: fill + 0.03
    property real borderAlpha: SelfvoyTheme.glassBorder
    property real base: 1.0
    property bool edge: false             // true — заметная рамка
    property real edgeStrength: 0
    property bool hovered: false
    property bool pressed: false
    property bool showHighlight: false
    property real radius: SelfvoyTheme.radiusCard
    // Сплошной цвет поверхности (по умолчанию — карточка темы).
    property color color: SelfvoyTheme.surface

    default property alias content: holder.data

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.color
        border.width: root.edge || SelfvoyTheme.dark ? 1 : 0
        border.color: root.edge ? SelfvoyTheme.borderStrong : SelfvoyTheme.border
    }
    // Ховер и нажатие — лёгкая «чернильная» плёнка поверх цвета.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: SelfvoyTheme.glass(root.pressed ? 0.08 : (root.hovered ? 0.04 : 0))
        Behavior on color { ColorAnimation { duration: SelfvoyTheme.pressMs } }
    }

    Item {
        id: holder
        anchors.fill: parent
    }
}
