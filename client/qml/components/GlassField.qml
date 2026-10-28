import QtQuick
import QtQuick.Controls
import Selfvoy
import "../icons/Icons.js" as Icons

/*
  GlassField — поле ввода 2026: серая подложка (как .input в вебе), акцентное
  кольцо в фокусе, иконка справа или кликабельное действие (глаз).

  label — подпись внутри поля сверху (формы настроек); на экранах входа подпись
  стоит над полем отдельным Text, а label пустой.
*/
TextField {
    id: control

    property string label: ""
    property bool invalid: false
    property string icon: ""
    property string action: ""
    property bool filled: false   // совместимость: все поля теперь с подложкой
    signal actionClicked()

    readonly property string rightIcon: {
        const name = action.length > 0 ? action : icon
        return Icons.paths[name] !== undefined ? name : Icons.iconFor(name)
    }

    implicitHeight: label.length > 0 ? 54 : 44
    implicitWidth: 240
    leftPadding: 14
    rightPadding: rightIcon.length > 0 ? 14 + 26 : 14
    topPadding: label.length > 0 ? 20 : 0
    bottomPadding: label.length > 0 ? 6 : 0
    font.family: SelfvoyTheme.fontFamily
    font.pixelSize: SelfvoyTheme.fontBody
    color: SelfvoyTheme.text
    selectionColor: SelfvoyTheme.accentA(0.3)
    selectedTextColor: SelfvoyTheme.text
    placeholderTextColor: SelfvoyTheme.text3
    verticalAlignment: TextInput.AlignVCenter
    focusPolicy: Qt.StrongFocus

    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: SelfvoyTheme.radiusField + 3
            color: "transparent"
            border.width: 2
            border.color: control.activeFocus ? SelfvoyTheme.accentA(0.45) : "transparent"
        }
        Rectangle {
            anchors.fill: parent
            radius: SelfvoyTheme.radiusField
            color: control.activeFocus ? SelfvoyTheme.surface : SelfvoyTheme.field
            border.width: control.invalid || control.activeFocus ? 1 : 0
            border.color: control.invalid ? SelfvoyTheme.danger : SelfvoyTheme.accent
        }
    }

    Text {
        x: control.leftPadding
        y: 8
        text: control.label
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontMicro
        font.weight: Font.DemiBold
        color: control.activeFocus ? SelfvoyTheme.accent : SelfvoyTheme.text2
        visible: control.label.length > 0
    }

    Icon {
        x: control.width - 14 - size
        anchors.verticalCenter: parent.verticalCenter
        name: control.rightIcon
        size: 18
        color: control.action.length > 0 && hit.containsMouse ? SelfvoyTheme.text : SelfvoyTheme.text2
        visible: control.rightIcon.length > 0

        MouseArea {
            id: hit
            anchors.fill: parent
            anchors.margins: -8
            enabled: control.action.length > 0
            hoverEnabled: true
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.IBeamCursor
            onClicked: control.actionClicked()
        }
    }
}
