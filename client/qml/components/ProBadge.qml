import QtQuick
import Selfvoy

// ProBadge — метка тарифа «PRO» (функция Pro или действующий Pro в профиле).
Rectangle {
    implicitWidth: label.implicitWidth + 12
    implicitHeight: 18
    radius: 6
    color: SelfvoyTheme.text
    Text {
        id: label
        anchors.centerIn: parent
        text: "PRO"
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: 0.4
        color: SelfvoyTheme.bg
    }
}
