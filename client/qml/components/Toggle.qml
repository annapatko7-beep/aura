import QtQuick
import QtQuick.Controls
import Selfvoy

// Toggle — переключатель в духе iOS: зелёный «включено», серый «выключено».
Switch {
    id: sw
    padding: 0
    implicitWidth: 46
    implicitHeight: 28
    indicator: Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: sw.checked ? SelfvoyTheme.toggleOn : SelfvoyTheme.fieldHover
        opacity: sw.enabled ? 1 : 0.5
        Behavior on color { ColorAnimation { duration: SelfvoyTheme.pressMs } }
        Rectangle {
            width: 24; height: 24; radius: 12
            y: 2
            x: sw.checked ? parent.width - width - 2 : 2
            color: "#FFFFFF"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.06)
            Behavior on x { NumberAnimation { duration: SelfvoyTheme.pressMs; easing.type: SelfvoyTheme.ease } }
        }
    }
    contentItem: null
}
