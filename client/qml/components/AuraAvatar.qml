import QtQuick

/*
  AuraAvatar — круглый аватар с инициалами и градиентом «северного сияния».
  agent: true — обводка и значок Ауры.
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
        if (parts.length === 1) return parts[0].substring(0, 2).toUpperCase()
        return (parts[0].substring(0, 1) + parts[1].substring(0, 1)).toUpperCase()
    }

    Rectangle {
        id: circle
        anchors.fill: parent
        radius: width / 2
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.agent ? "#6E7BFF" : "#3A4152" }
            GradientStop { position: 1.0; color: root.agent ? "#3E4B8C" : "#232833" }
        }
        border.width: 1
        border.color: root.agent ? Qt.rgba(1, 1, 1, 0.35) : Qt.rgba(1, 1, 1, 0.12)
    }

    Text {
        anchors.centerIn: parent
        text: root.initials
        font.family: AuraTheme.fontFamily
        font.pixelSize: Math.max(10, root.size * 0.36)
        font.weight: Font.DemiBold
        color: AuraTheme.textPrimary
    }

    // Индикатор «онлайн»
    Rectangle {
        width: Math.max(8, root.size * 0.26)
        height: width
        radius: width / 2
        color: AuraTheme.online
        border.width: 2
        border.color: AuraTheme.bgBase
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.online
    }
}
