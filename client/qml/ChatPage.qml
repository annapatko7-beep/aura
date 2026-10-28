import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy

/*
  ChatPage — переписка с человеком (открывается из «Контактов»).

  Шапка с собеседником, лента сообщений (сообщения Selfvoy — agent_reply/agent_action —
  со значком Selfvoy) и одна строка ввода: микрофон, текст, «Спросить Selfvoy» (agent.ask
  в этом чате — Selfvoy сам предложит переговоры) или обычная отправка.
*/
Item {
    id: page

    property int chatId: 0
    property bool embedded: false
    property bool toSelfvoy: false

    signal back()
    signal openSettings()

    Component.onCompleted: if (page.chatId > 0) App.selectChat(page.chatId)
    onChatIdChanged: if (page.chatId > 0) App.selectChat(page.chatId)

    function send() {
        const text = input.text.trim()
        if (!text || !App || App.busy) return
        if (page.toSelfvoy) App.askAgent(page.chatId, text)
        else App.sendMessage(page.chatId, text)
        input.text = ""
    }

    Connections {
        target: App
        function onMessagesChanged() { Qt.callLater(() => messageList.positionViewAtEnd()) }
        function onTranscriptChanged() { if (App.transcript) input.text = App.transcript }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // ---------------------------------------------------------- шапка
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            GlassButton {
                variant: "quiet"
                implicitWidth: 36; implicitHeight: 36
                glyphSize: 16
                symbol: "arrow-left"
                onClicked: page.back()
            }
            SelfvoyAvatar { name: App ? App.chatTitle : ""; size: 40; online: App ? App.peerOnline : false }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: App ? App.chatTitle : ""
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontLead
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: App && App.peerOnline ? "в сети" : "не в сети — Selfvoy ответит за него"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontMicro + 1
                    color: App && App.peerOnline ? SelfvoyTheme.success : SelfvoyTheme.text3
                }
            }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: SelfvoyTheme.glass(0.08) }

        // ---------------------------------------------------------- лента
        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: SelfvoyTheme.spaceMd
            model: App ? App.messages : []
            boundsBehavior: Flickable.StopAtBounds

            Text {
                anchors.centerIn: parent
                visible: messageList.count === 0
                width: parent.width
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                text: "Сообщений пока нет. Напишите или попросите Selfvoy договориться о встрече."
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                color: SelfvoyTheme.text3
            }

            delegate: ChatBubble {
                width: messageList.width
                text: modelData ? String(modelData.body || "") : ""
                senderName: modelData ? String(modelData.sender_name || "") : ""
                timestamp: modelData ? String(modelData.created_at || "").substring(11, 16) : ""
                mine: modelData ? modelData.sender_id === App.userId : false
                agent: modelData ? String(modelData.kind || "").indexOf("agent") === 0 : false
                maxBubbleWidth: Math.max(220, messageList.width * 0.78)
            }

            footer: Item {
                width: messageList.width
                height: App && App.busy ? 30 : 0
                visible: App && App.busy
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Selfvoy думает…"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall
                    color: SelfvoyTheme.text2
                }
            }
        }

        // ---------------------------------------------------------- ввод
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Chip {
                text: "Обычное"
                active: !page.toSelfvoy
                onClicked: page.toSelfvoy = false
            }
            Chip {
                text: "Спросить Selfvoy"
                active: page.toSelfvoy
                onClicked: page.toSelfvoy = true
            }
            Item { Layout.fillWidth: true }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            GlassButton {
                implicitWidth: 44; implicitHeight: 44
                glyphSize: 18
                symbol: App && App.voiceState === "listening" ? "square" : "mic"
                busy: App ? App.voiceState === "processing" : false
                enabled: App ? App.micAvailable : false
                onClicked: App.voiceState === "listening" ? App.stopListening() : App.startListening()
            }
            GlassField {
                id: input
                Layout.fillWidth: true
                placeholderText: page.toSelfvoy ? "Например: договорись о встрече на выходных" : "Сообщение…"
                onAccepted: page.send()
            }
            GlassButton {
                variant: "accent"
                implicitWidth: 46; implicitHeight: 46
                glyphSize: 18
                symbol: page.toSelfvoy ? "sparkles" : "send-horizontal"
                busy: App ? App.busy : false
                enabled: input.text.trim().length > 0 && App && !App.busy
                onClicked: page.send()
            }
        }
    }
}
