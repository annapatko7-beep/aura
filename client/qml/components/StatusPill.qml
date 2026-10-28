import QtQuick
import Selfvoy

/*
  StatusPill — статус всегда значком и словом (как .status-pill в вебе), не только
  цветом. status — объект {label, tone, icon} из Format.js; tone: green | amber |
  new | red | accent | "" (нейтральный). «new» — янтарный знак «новое».
*/
Rectangle {
    id: root
    property var status: null
    property string label: status ? status.label : ""
    property string tone: status ? status.tone : ""
    property string icon: status ? status.icon : ""

    visible: label.length > 0
    implicitHeight: 24
    implicitWidth: row.implicitWidth + 16
    radius: 12
    color: SelfvoyTheme.toneBg(tone)

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        Item {
            width: 15; height: 15
            anchors.verticalCenter: parent.verticalCenter
            visible: root.icon.length > 0
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                visible: root.tone === "new"
                color: SelfvoyTheme.amber
            }
            Icon {
                anchors.centerIn: parent
                name: root.icon
                size: root.tone === "new" ? 11 : 14
                color: root.tone === "new" ? "#FFFFFF" : SelfvoyTheme.toneFg(root.tone)
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: SelfvoyTheme.toneFg(root.tone)
        }
    }
}
