import QtQuick
import Selfvoy
import "../icons/Icons.js" as Icons

/*
  Icon — линейная иконка Lucide (ISC) любого цвета.

  Разметка иконок лежит в icons/Icons.js; цвет подставляется в штрих SVG,
  и картинка отдаётся Image как data-URL — без растровых копий на каждый цвет.
*/
Item {
    id: root

    property string name: ""
    property color color: SelfvoyTheme.textSecondary
    property int size: 18

    implicitWidth: size
    implicitHeight: size

    Image {
        anchors.fill: parent
        sourceSize: Qt.size(root.size * 2, root.size * 2)
        smooth: true
        source: root.name ? "data:image/svg+xml;utf8," + encodeURIComponent(Icons.svg(root.name, root.color.toString()))
                          : ""
    }
}
