import QtQuick
import Selfvoy

/*
  Segmented — сегментированный переключатель (как Segmented в вебе).
  options — [{value, label}], value — выбранное; activated(value) — выбор.
*/
Rectangle {
    id: root
    property var options: []
    property var value
    signal activated(var value)

    implicitHeight: 34
    implicitWidth: row.implicitWidth + 4
    radius: 9
    color: SelfvoyTheme.field

    Row {
        id: row
        x: 2; y: 2
        spacing: 2
        Repeater {
            model: root.options
            delegate: Rectangle {
                required property var modelData
                readonly property bool on: root.value === modelData.value
                width: Math.max(64, label.implicitWidth + 24)
                height: root.height - 4
                radius: 7
                color: on ? (SelfvoyTheme.dark ? "#636366" : "#FFFFFF") : (hit.containsMouse ? SelfvoyTheme.glass(0.04) : "transparent")
                border.width: on && !SelfvoyTheme.dark ? 1 : 0
                border.color: SelfvoyTheme.border
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData.label
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    font.weight: parent.on ? Font.DemiBold : Font.Medium
                    color: SelfvoyTheme.text
                }
                MouseArea {
                    id: hit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activated(modelData.value)
                }
            }
        }
    }
}
