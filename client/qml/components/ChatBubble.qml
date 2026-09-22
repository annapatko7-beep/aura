import QtQuick

/*
  ChatBubble — «пузырь» сообщения.

  Свой текст — справа, акцентное стекло; чужой — слева, нейтральное стекло;
  сообщения Ауры помечаются значком и холодным оттенком.
*/
Item {
    id: root

    property string text: ""
    property string senderName: ""
    property string timestamp: ""
    property bool mine: false
    property bool agent: false
    property real maxBubbleWidth: 520

    implicitHeight: column.height + (root.agent ? badge.height + 6 : 0)
    implicitWidth: parent ? parent.width : maxBubbleWidth
    height: implicitHeight

    readonly property color bubbleTint: root.agent ? AuraTheme.accentSoft
                                                 : (root.mine ? AuraTheme.accent : AuraTheme.glassTint)
    readonly property real bubbleFill: root.agent ? 0.16 : (root.mine ? AuraTheme.accentFill : AuraTheme.glassFill)

    // Значок Ауры над сообщением агента
    Row {
        id: badge
        anchors.left: root.mine ? undefined : parent.left
        anchors.right: root.mine ? parent.right : undefined
        anchors.leftMargin: AuraTheme.spaceXs
        anchors.rightMargin: AuraTheme.spaceXs
        spacing: 5
        visible: root.agent
        height: visible ? 16 : 0

        Rectangle {
            width: 6
            height: 6
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: AuraTheme.accentSoft
        }
        Text {
            text: "Аура"
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontMicro
            font.weight: Font.DemiBold
            color: AuraTheme.accentSoft
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Item {
        id: column
        anchors.top: badge.bottom
        anchors.topMargin: root.agent ? 6 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        height: bubble.height

        Rectangle {
            id: bubble
            anchors.right: root.mine ? parent.right : undefined
            anchors.left: root.mine ? undefined : parent.left
            width: Math.min(root.maxBubbleWidth, Math.max(80, bubbleText.implicitWidth + AuraTheme.spaceLg * 2))
            height: bubbleText.implicitHeight + AuraTheme.spaceLg
            radius: AuraTheme.radiusBubble
            color: Qt.rgba(root.bubbleTint.r, root.bubbleTint.g, root.bubbleTint.b, root.bubbleFill)
            border.width: 1
            border.color: root.agent
                          ? AuraTheme.accentA(0.35)
                          : Qt.rgba(1, 1, 1, root.mine ? 0.22 : AuraTheme.glassBorder)

            // Блик по верхнему краю пузыря
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 1
                height: parent.height * 0.45
                radius: parent.radius
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.16) }
                    GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
                }
            }

            Text {
                id: bubbleText
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: AuraTheme.spaceLg
                text: root.text
                wrapMode: Text.WordWrap
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontBody
                color: AuraTheme.textPrimary
                lineHeight: 1.25
            }
        }
    }

    // Подпись: кто и когда
    Text {
        anchors.top: column.bottom
        anchors.topMargin: 4
        anchors.left: root.mine ? undefined : parent.left
        anchors.right: root.mine ? parent.right : undefined
        anchors.leftMargin: AuraTheme.spaceXs
        anchors.rightMargin: AuraTheme.spaceXs
        text: [root.senderName, root.timestamp].filter(part => part.length > 0).join(" · ")
        font.family: AuraTheme.fontFamily
        font.pixelSize: AuraTheme.fontMicro
        color: AuraTheme.textMuted
        visible: text.length > 0
    }
}
