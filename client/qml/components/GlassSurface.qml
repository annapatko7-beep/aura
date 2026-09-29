import QtQuick
import QtQuick.Effects

/*
  GlassSurface — базовая «стеклянная» поверхность.

  Сверху на размытый фон кладётся полупрозрачная плёнка с тонкой светлой
  рамкой и бликом по верхнему краю. Если backdrop не задан, остаётся просто
  полупрозрачная панель (например, на сплошном фоне списка).
*/
Item {
    id: root

    // Что размывать под стеклом (обычно — фон окна).
    property Item backdrop: null
    // Сила размытия и прозрачности.
    property real blurStrength: 48
    property color tint: AuraTheme.glassTint
    property real fill: AuraTheme.glassFill
    property real fillHover: fill
    property real borderAlpha: AuraTheme.glassBorder
    property bool hovered: false
    property bool pressed: false
    property bool showHighlight: true
    property alias radius: film.radius
    property alias border: film.border

    default property alias content: holder.data

    readonly property bool blurred: backdrop !== null && backdrop.width > 0

    // Область фона под панелью в координатах backdrop.
    readonly property point origin: backdrop ? mapToItem(backdrop, 0, 0) : Qt.point(0, 0)

    ShaderEffectSource {
        id: backdropSource
        anchors.fill: parent
        visible: false
        live: true
        sourceItem: root.backdrop
        sourceRect: root.blurred
                    ? Qt.rect(root.origin.x, root.origin.y, root.width, root.height)
                    : Qt.rect(0, 0, 0, 0)
    }

    // Маска со скруглёнными углами, чтобы стекло не выходило за радиус.
    Item {
        id: cornerMask
        anchors.fill: parent
        visible: false
        layer.enabled: root.blurred
        layer.smooth: true
        Rectangle {
            anchors.fill: parent
            radius: film.radius
            color: "#ffffff"
        }
    }

    MultiEffect {
        id: blur
        anchors.fill: parent
        visible: root.blurred
        source: backdropSource
        blurEnabled: true
        blur: 1.0
        blurMax: root.blurStrength
        maskEnabled: true
        maskSource: cornerMask
        opacity: 0.9
    }

    // Плёнка стекла: полупрозрачный белый + рамка.
    Rectangle {
        id: film
        anchors.fill: parent
        radius: AuraTheme.radiusCard
        color: Qt.rgba(root.tint.r, root.tint.g, root.tint.b,
                       root.pressed ? AuraTheme.glassFillPressed
                                    : (root.hovered ? root.fillHover : root.fill))
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, root.pressed ? root.borderAlpha + 0.1 : root.borderAlpha)

        Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }
        Behavior on border.color { ColorAnimation { duration: AuraTheme.pressMs } }
    }

    // Внутренний блик сверху — он и даёт ощущение объёма.
    Rectangle {
        id: highlight
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: Math.max(1, Math.round(parent.height * 0.42))
        radius: film.radius
        opacity: root.showHighlight ? 1 : 0
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, AuraTheme.glassHighlight) }
            GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
        }
        Behavior on opacity { NumberAnimation { duration: AuraTheme.pressMs } }
    }

    Item {
        id: holder
        anchors.fill: parent
    }
}
