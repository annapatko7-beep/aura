import QtQuick
import QtQuick.Layouts
import Selfvoy

/*
  Group — сгруппированный список настроек (как .group в вебе): подпись сверху
  капсом, белая карточка с ячейками (Cell), пояснение снизу.
*/
ColumnLayout {
    id: root
    property string head: ""
    property string foot: ""
    property color footColor: SelfvoyTheme.text2
    property Component headExtra: null
    default property alias cells: body.data

    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 16
        Layout.rightMargin: 16
        visible: root.head.length > 0
        spacing: 8
        Text {
            text: root.head.toUpperCase()
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            font.letterSpacing: 0.3
            color: SelfvoyTheme.text2
        }
        Loader { sourceComponent: root.headExtra; active: root.headExtra !== null }
        Item { Layout.fillWidth: true }
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight
        radius: SelfvoyTheme.radiusCard
        color: SelfvoyTheme.surface
        border.width: SelfvoyTheme.dark ? 1 : 0
        border.color: SelfvoyTheme.border
        clip: true
        ColumnLayout {
            id: body
            width: parent.width
            spacing: 0
        }
    }
    Text {
        Layout.fillWidth: true
        Layout.leftMargin: 16
        Layout.rightMargin: 16
        visible: root.foot.length > 0
        text: root.foot
        wrapMode: Text.WordWrap
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontSmall
        color: root.footColor
    }
}
