import QtQuick
import Selfvoy

// SelfvoyOrb — аватар ассистента Selfvoy: знак бренда на мягком акцентном круге.
Item {
    id: root
    property int size: 70
    property bool animate: false
    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: SelfvoyTheme.accentTint
    }
    SelfvoyLogo {
        anchors.centerIn: parent
        size: Math.round(root.size * 0.6)
    }
}
