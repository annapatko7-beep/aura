import QtQuick
import QtQuick.Controls

/*
  GlassField — стеклянное поле ввода с плавающей подсказкой.
*/
TextField {
    id: control

    property string label: ""
    property bool invalid: false

    implicitHeight: 52
    implicitWidth: 240
    leftPadding: AuraTheme.spaceMd
    rightPadding: AuraTheme.spaceMd
    topPadding: label.length > 0 ? 20 : 0
    bottomPadding: label.length > 0 ? 6 : 0
    font.family: AuraTheme.fontFamily
    font.pixelSize: AuraTheme.fontBody
    color: AuraTheme.textPrimary
    selectionColor: AuraTheme.accentA(0.5)
    selectedTextColor: AuraTheme.textPrimary
    placeholderTextColor: AuraTheme.textMuted
    verticalAlignment: TextInput.AlignVCenter
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: AuraTheme.radiusField
        color: Qt.rgba(1, 1, 1, control.activeFocus ? AuraTheme.glassFillPressed : AuraTheme.glassFill)
        border.width: 1
        border.color: control.invalid
                      ? AuraTheme.danger
                      : (control.activeFocus ? AuraTheme.accentA(0.7) : Qt.rgba(1, 1, 1, AuraTheme.glassBorder))

        Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }
        Behavior on border.color { ColorAnimation { duration: AuraTheme.pressMs } }

        // Блик сверху
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: parent.height * 0.45
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.10) }
                GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
            }
        }
    }

    // Плавающая подпись внутри поля
    Text {
        x: control.leftPadding
        y: 8
        text: control.label
        font.family: AuraTheme.fontFamily
        font.pixelSize: AuraTheme.fontMicro
        color: control.activeFocus ? AuraTheme.accent : AuraTheme.textMuted
        visible: control.label.length > 0
        Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }
    }
}
