import QtQuick
import QtQuick.Controls
import Selfvoy
import "../icons/Icons.js" as Icons

/*
  GlassButton — кнопка 2026 (как .btn в вебе). Имя осталось от стеклянной темы.

  variant: "accent"  — сплошной акцент #3555F5, белый текст (главное действие)
           "glass"   — вторичная: мягкий акцентный фон, акцентный текст (по умолчанию)
           "neutral" — серая подложка поля, обычный текст (иконки-кнопки)
           "quiet"   — без фона, акцентный текст; фон только при наведении
           "danger"  — мягкий красный фон, красный текст
           "onColor" — на цветной карточке: белая плёнка 20 % и белый текст
  symbol — имя иконки Lucide (Icon.qml); glyph — текстовый символ (совместимость).
*/
Button {
    id: control

    property string variant: "glass"
    property string glyph: ""
    property string symbol: ""
    property bool busy: false
    property real glyphSize: 16
    property real radius: SelfvoyTheme.radiusPill
    property int textSize: SelfvoyTheme.fontBody
    property bool alignLeft: false

    readonly property string resolvedIcon: symbol.length > 0 ? symbol : Icons.iconFor(glyph)
    readonly property bool isAccent: variant === "accent"
    readonly property color fg: isAccent || variant === "onColor" ? "#FFFFFF"
                                : variant === "danger" ? SelfvoyTheme.danger
                                : variant === "neutral" ? SelfvoyTheme.text
                                : SelfvoyTheme.accent
    readonly property color face: {
        switch (variant) {
        case "accent": return control.hovered ? SelfvoyTheme.accentHover : SelfvoyTheme.accentSolid
        case "danger": return SelfvoyTheme.dangerSoft
        case "neutral": return control.hovered ? SelfvoyTheme.fieldHover : SelfvoyTheme.field
        case "onColor": return Qt.rgba(1, 1, 1, control.hovered ? 0.28 : 0.2)
        case "quiet": return control.hovered ? SelfvoyTheme.field : "transparent"
        default: return SelfvoyTheme.accentTint
        }
    }

    implicitHeight: 40
    implicitWidth: Math.max(implicitHeight, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: text.length > 0 ? 18 : 0
    rightPadding: text.length > 0 ? 18 : 0
    topPadding: 0
    bottomPadding: 0
    focusPolicy: Qt.TabFocus
    hoverEnabled: true

    scale: pressed ? SelfvoyTheme.pressScale : 1.0
    opacity: enabled ? 1.0 : 0.45
    Behavior on scale { NumberAnimation { duration: SelfvoyTheme.pressMs; easing.type: SelfvoyTheme.ease } }

    background: Item {
        implicitHeight: control.implicitHeight
        Rectangle {
            id: faceRect
            anchors.fill: parent
            radius: Math.min(control.radius, height / 2)
            color: control.face
            border.width: control.variant === "onColor" ? 1 : 0
            border.color: Qt.rgba(1, 1, 1, 0.35)
            Behavior on color { ColorAnimation { duration: SelfvoyTheme.pressMs } }
            // Тонкая «чернильная» плёнка при нажатии (вторичные кнопки)
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                visible: control.pressed && !control.isAccent
                color: SelfvoyTheme.glass(0.06)
            }
        }
        // Индикатор фокуса с клавиатуры
        Rectangle {
            anchors.fill: faceRect
            anchors.margins: -3
            radius: faceRect.radius + 3
            color: "transparent"
            border.width: control.visualFocus ? 2 : 0
            border.color: SelfvoyTheme.accentSolid
        }
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row
            spacing: 7
            anchors.verticalCenter: parent.verticalCenter
            x: control.alignLeft ? 0 : Math.max(0, (parent.width - width) / 2)

            Item {
                width: visible ? control.glyphSize + 2 : 0
                height: control.glyphSize + 2
                visible: control.resolvedIcon.length > 0 || control.glyph.length > 0 || control.busy
                anchors.verticalCenter: parent.verticalCenter

                Icon {
                    anchors.centerIn: parent
                    name: control.resolvedIcon
                    size: control.glyphSize
                    color: control.fg
                    visible: control.resolvedIcon.length > 0 && !control.busy
                }
                Text {
                    anchors.centerIn: parent
                    text: control.glyph
                    visible: control.resolvedIcon.length === 0 && !control.busy
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: control.glyphSize
                    color: control.fg
                }
                // Спиннер
                Rectangle {
                    anchors.centerIn: parent
                    width: control.glyphSize
                    height: control.glyphSize
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: Qt.rgba(control.fg.r, control.fg.g, control.fg.b, 0.3)
                    visible: control.busy
                    Rectangle {
                        width: 5; height: 5; radius: 2.5
                        color: control.fg
                        x: parent.width / 2 - 2.5
                        y: -1.5
                    }
                    RotationAnimation on rotation {
                        from: 0; to: 360; duration: 900
                        loops: Animation.Infinite
                        running: control.busy
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                visible: control.text.length > 0
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: control.textSize
                font.weight: Font.DemiBold
                color: control.fg
                elide: Text.ElideRight
            }
        }
    }
}
