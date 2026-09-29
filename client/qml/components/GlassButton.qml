import QtQuick
import QtQuick.Controls

/*
  GlassButton — «пузырьковая» кнопка: полупрозрачное стекло, скругление-пилюля,
  отклик нажатием (лёгкое уменьшение и подсветка), как в iOS.

  Варианты:
    variant: "glass"   — нейтральное стекло (по умолчанию)
             "accent"  — акцентное стекло с сиянием
             "quiet"   — почти прозрачная, для второстепенных действий
             "danger"  — красноватое стекло
*/
Button {
    id: control

    property string variant: "glass"
    property string glyph: ""
    property bool busy: false
    property real glyphSize: 15

    readonly property color baseColor: {
        switch (variant) {
        case "accent": return AuraTheme.accent
        case "danger": return AuraTheme.danger
        default:       return AuraTheme.glassTint
        }
    }
    readonly property real baseFill: {
        switch (variant) {
        case "accent": return AuraTheme.accentFill
        case "quiet":  return 0.03
        case "danger": return 0.18
        default:       return AuraTheme.glassFill
        }
    }

    implicitHeight: 46
    implicitWidth: Math.max(120, contentItem.implicitWidth + AuraTheme.spaceLg * 2)
    leftPadding: AuraTheme.spaceLg
    rightPadding: AuraTheme.spaceLg
    topPadding: AuraTheme.spaceMd
    bottomPadding: AuraTheme.spaceMd
    focusPolicy: Qt.TabFocus

    // Отклик нажатия: мягкое «вдавливание».
    scale: pressed ? AuraTheme.pressScale : 1.0
    opacity: enabled ? 1.0 : 0.45
    // Приподнимаемся на 1 px при наведении (translateY(-1px)).
    y: hovered && !pressed ? AuraTheme.hoverLift : 0
    Behavior on scale {
        NumberAnimation { duration: AuraTheme.pressMs; easing.type: AuraTheme.ease }
    }
    Behavior on y {
        NumberAnimation { duration: AuraTheme.pressMs; easing.type: AuraTheme.ease }
    }
    Behavior on opacity { NumberAnimation { duration: AuraTheme.moveMs; easing.type: AuraTheme.ease } }

    background: Item {
        implicitHeight: control.implicitHeight

        // Мягкая тень под кнопкой; акцент получает фиолетовое свечение
        Rectangle {
            anchors.fill: glass
            anchors.margins: control.variant === "accent" ? -10 : -6
            radius: glass.radius + (control.variant === "accent" ? 10 : 6)
            color: control.variant === "accent" ? AuraTheme.accentGlow : AuraTheme.shadowColor
            opacity: control.variant === "accent" ? 0.9 : 0.35
            visible: control.variant !== "quiet"
        }

        Rectangle {
            id: glass
            anchors.fill: parent
            radius: AuraTheme.radiusPill
            color: control.variant === "accent"
                   ? (control.pressed ? AuraTheme.accentDeep
                                      : (control.hovered ? Qt.lighter(AuraTheme.accent, 1.08) : AuraTheme.accent))
                   : Qt.rgba(control.baseColor.r, control.baseColor.g, control.baseColor.b,
                             control.pressed ? control.baseFill + 0.06
                                             : (control.hovered ? control.baseFill + 0.04
                                                                : control.baseFill))
            border.width: 1
            border.color: control.variant === "accent"
                          ? Qt.rgba(1, 1, 1, control.pressed ? 0.42 : 0.28)
                          : Qt.rgba(1, 1, 1, control.pressed ? AuraTheme.glassBorder + 0.10
                                                             : AuraTheme.glassBorder)

            Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }
            Behavior on border.color { ColorAnimation { duration: AuraTheme.pressMs } }

            // Верхний блик
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: parent.height * 0.5
                radius: parent.radius
                gradient: Gradient {
                    GradientStop {
                        position: 0.0
                        color: Qt.rgba(1, 1, 1, control.variant === "quiet" ? 0.06 : 0.18)
                    }
                    GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
                }
            }
        }

        // Индикатор фокуса с клавиатуры
        Rectangle {
            anchors.fill: glass
            anchors.margins: -3
            radius: glass.radius + 3
            color: "transparent"
            border.width: control.visualFocus ? 2 : 0
            border.color: AuraTheme.accentA(0.6)
        }
    }

    contentItem: Row {
        spacing: AuraTheme.spaceSm
        topPadding: 0
        bottomPadding: 0

        Item {
            width: control.glyph.length > 0 ? control.glyphSize + 4 : 0
            height: control.glyphSize + 4
            visible: control.glyph.length > 0
            anchors.verticalCenter: parent.verticalCenter

            Text {
                anchors.centerIn: parent
                text: control.glyph
                font.pixelSize: control.glyphSize
                color: control.variant === "accent" ? AuraTheme.textOnAccent : AuraTheme.textPrimary
                opacity: control.busy ? 0.4 : 1.0
            }

            // «Спиннер» поверх глифа, пока идёт запрос
            Rectangle {
                anchors.centerIn: parent
                width: control.glyphSize
                height: control.glyphSize
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: AuraTheme.accentA(0.25)
                visible: control.busy
            }
            Rectangle {
                anchors.centerIn: parent
                width: control.glyphSize
                height: control.glyphSize
                radius: width / 2
                color: "transparent"
                border.width: 2
                border.color: control.variant === "accent" ? AuraTheme.textOnAccent : AuraTheme.accent
                visible: control.busy
                // Дуга: скрываем часть окружности и вращаем
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: parent.height
                    radius: width / 2
                    color: "transparent"
                }
                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: control.busy
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontBody
            font.weight: control.variant === "accent" ? Font.DemiBold : Font.Medium
            color: control.variant === "accent" ? AuraTheme.textOnAccent : AuraTheme.textPrimary
            elide: Text.ElideRight
        }
    }
}
