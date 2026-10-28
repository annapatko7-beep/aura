import QtQuick
import Selfvoy

/*
  ChatBubble — «пузырь» сообщения.

  Свой текст — справа, мягкий акцент; чужой — слева, сплошная карточка;
  сообщения Selfvoy помечаются значком и холодным оттенком.
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

    // Свои — мягкий акцент, Selfvoy и собеседник — сплошная карточка.
    readonly property color bubbleColor: root.mine ? SelfvoyTheme.accentTint : SelfvoyTheme.surface

    // Значок Selfvoy над сообщением агента
    Row {
        id: badge
        anchors.left: root.mine ? undefined : parent.left
        anchors.right: root.mine ? parent.right : undefined
        anchors.leftMargin: SelfvoyTheme.spaceXs
        anchors.rightMargin: SelfvoyTheme.spaceXs
        spacing: 5
        visible: root.agent
        height: visible ? 16 : 0

        Rectangle {
            width: 6
            height: 6
            radius: 3
            anchors.verticalCenter: parent.verticalCenter
            color: SelfvoyTheme.accentSoft
        }
        Text {
            text: "Selfvoy"
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontMicro
            font.weight: Font.DemiBold
            color: SelfvoyTheme.accentSoft
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
            // Ширина по копии текста без переноса (как в Bubble.qml): implicitWidth
            // самого переносимого текста зависит от уже выставленной ширины.
            width: Math.min(root.maxBubbleWidth, Math.max(80, Math.ceil(bubbleProbe.implicitWidth) + 2 + SelfvoyTheme.spaceLg * 2))
            height: bubbleText.implicitHeight + SelfvoyTheme.spaceLg
            radius: SelfvoyTheme.radiusBubble

            Text {
                id: bubbleProbe
                visible: false
                text: root.text
                font: bubbleText.font
            }
            color: root.bubbleColor
            border.width: root.agent ? 1 : 0
            border.color: SelfvoyTheme.accentA(0.35)

            Text {
                id: bubbleText
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: SelfvoyTheme.spaceLg
                text: root.text
                wrapMode: Text.WordWrap
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: SelfvoyTheme.textPrimary
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
        anchors.leftMargin: SelfvoyTheme.spaceXs
        anchors.rightMargin: SelfvoyTheme.spaceXs
        text: [root.senderName, root.timestamp].filter(part => part.length > 0).join(" · ")
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontMicro
        color: SelfvoyTheme.textMuted
        visible: text.length > 0
    }
}
