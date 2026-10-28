import QtQuick
import Selfvoy

/*
  SelfvoyAvatar — круглый аватар с инициалами на мягком оттенке (как Avatar в вебе).
  agent: true — обводка и значок Selfvoy.
*/
Item {
    id: root

    property string name: ""
    property bool agent: false
    property bool online: false
    property int size: 38

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    readonly property string initials: {
        const parts = name.trim().split(/\s+/).filter(part => part.length > 0)
        if (parts.length === 0) return agent ? "AI" : "?"
        if (parts.length === 1) return parts[0].substring(0, 1).toUpperCase()
        return (parts[0].substring(0, 1) + parts[1].substring(0, 1)).toUpperCase()
    }

    // Оттенок — из имени: стабильный между запусками; тёмный текст на нём читается.
    readonly property var hues: ["#CFE0FF", "#FFE2B3", "#D7F2E2", "#E9DDFF", "#FFD9E5", "#FFD8C7", "#D9F0F7"]
    property color tone: {
        if (agent) return SelfvoyTheme.accentTint
        let sum = 0
        for (let i = 0; i < name.length; ++i) sum += name.charCodeAt(i)
        return hues[sum % hues.length]
    }
    property bool ring: false

    Rectangle {
        anchors.fill: parent
        anchors.margins: -5
        radius: width / 2
        visible: root.ring
        color: "transparent"
        border.width: 3
        border.color: SelfvoyTheme.accentA(0.25)
    }

    Rectangle {
        id: circle
        anchors.fill: parent
        radius: width / 2
        color: root.tone
    }

    Text {
        anchors.centerIn: parent
        text: root.initials
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: Math.max(10, root.size * 0.36)
        font.weight: Font.DemiBold
        color: root.agent ? SelfvoyTheme.accent : "#1C1C1E"
    }

    // Индикатор «онлайн»
    Rectangle {
        width: Math.max(8, root.size * 0.26)
        height: width
        radius: width / 2
        color: SelfvoyTheme.online
        border.width: 2
        border.color: SelfvoyTheme.surface
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.online
    }
}
