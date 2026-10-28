import QtQuick
import Selfvoy

/*
  SelfvoyEmblem — крупный знак Selfvoy (экран входа): S-линия между точками.
  Свойства animate/dashes/glow оставлены для совместимости, без свечения.
*/
Item {
    id: root

    property int size: 72
    property int dashes: 12
    property bool animate: false
    property real glow: 0

    implicitWidth: size
    implicitHeight: size

    SelfvoyLogo {
        anchors.centerIn: parent
        size: root.size
    }
}
