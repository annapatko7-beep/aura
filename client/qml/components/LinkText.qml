import QtQuick
import Selfvoy

// LinkText — текстовая ссылка («Все задачи», «Создать аккаунт»).
Text {
    id: root
    signal clicked()
    property color normalColor: SelfvoyTheme.accent
    font.family: SelfvoyTheme.fontFamily
    font.pixelSize: SelfvoyTheme.fontSmall
    font.weight: Font.Medium
    color: normalColor
    font.underline: area.containsMouse
    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
