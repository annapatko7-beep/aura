import QtQuick
import QtQuick.Controls
import Selfvoy

// Chip — пилюля-кнопка: быстрые ответы, фильтры, варианты окна переговоров.
AbstractButton {
    id: control
    property bool active: false
    hoverEnabled: true
    implicitHeight: 34
    implicitWidth: label.implicitWidth + 30
    opacity: enabled ? 1 : 0.55

    background: Rectangle {
        radius: height / 2
        color: control.active ? SelfvoyTheme.accentTint
               : control.hovered || control.pressed ? SelfvoyTheme.fieldHover : SelfvoyTheme.field
        border.width: control.active ? 1 : 0
        border.color: SelfvoyTheme.accent
        Behavior on color { ColorAnimation { duration: SelfvoyTheme.pressMs } }
    }
    contentItem: Text {
        id: label
        text: control.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontSmall
        font.weight: Font.DemiBold
        color: control.active ? SelfvoyTheme.accent : SelfvoyTheme.text
        elide: Text.ElideRight
    }
}
