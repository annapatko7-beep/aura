import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  ChatPage — диалог с человеком и с его Аурой.

  Обычное сообщение уходит в чат (chat.send), а «Спросить Ауру» отправляет
  запрос агенту (agent.ask): он сам напишет собеседнику и договорится о встрече.
*/
Item {
    id: page

    property int chatId: 0
    // true — страница встроена в desktop-оболочку (master-detail): без кнопок
    // «назад»/«настройки» (навигация — через рельс оболочки).
    property bool embedded: false

    signal back()
    signal openSettings()

    // История чата грузится при открытии и при смене выбранного чата
    // (раньше selectChat не вызывался — существующие чаты открывались пустыми).
    Component.onCompleted: if (page.chatId > 0) App.selectChat(page.chatId)
    onChatIdChanged: if (page.chatId > 0) App.selectChat(page.chatId)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: AuraTheme.spaceLg
        spacing: AuraTheme.spaceMd

        // -------------------------------------------------------- шапка
        GlassPanel {
            Layout.fillWidth: true
            padding: AuraTheme.spaceMd
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            RowLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceMd

                GlassButton {
                    visible: !page.embedded
                    variant: "quiet"
                    text: ""
                    glyph: "←"
                    implicitWidth: 44
                    implicitHeight: 40
                    onClicked: page.back()
                }

                AuraAvatar {
                    name: App ? App.chatTitle : ""
                    size: 40
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: App ? App.chatTitle : "Чат"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontTitle
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        text: App && App.peerOnline ? "в сети · Аура активна" : "не в сети"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: App && App.peerOnline ? AuraTheme.online : AuraTheme.textMuted
                    }
                }

                GlassButton {
                    visible: !page.embedded
                    variant: "quiet"
                    glyph: "⚙"
                    text: ""
                    implicitWidth: 44
                    implicitHeight: 40
                    onClicked: page.openSettings()
                }
            }
        }

        // ------------------------------------------------------ сообщения
        GlassPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: AuraTheme.spaceMd
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            ListView {
                id: messageList
                anchors.fill: parent
                clip: true
                spacing: AuraTheme.spaceMd
                model: App ? App.messages : []
                boundsBehavior: Flickable.StopAtBounds

                Text {
                    anchors.centerIn: parent
                    visible: messageList.count === 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "Напишите сообщение или попросите Ауру договориться о встрече."
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textMuted
                }

                delegate: ChatBubble {
                    width: messageList.width
                    text: modelData ? String(modelData.body || "") : ""
                    senderName: modelData ? String(modelData.sender_name || "") : ""
                    timestamp: modelData ? String(modelData.created_at || "").substring(11, 16) : ""
                    mine: modelData ? modelData.sender_id === App.userId : false
                    agent: modelData ? String(modelData.kind || "").indexOf("agent") === 0 : false
                    maxBubbleWidth: Math.max(220, messageList.width * 0.68)
                }

                onCountChanged: Qt.callLater(positionViewAtEnd)
            }
        }

        // ------------------------------------------ подтверждение действий Ауры
        ConfirmationPanel {
            Layout.fillWidth: true
            backdrop: page.Window.window ? page.Window.window.contentItem : null
        }

        // ------------------------------------------------------- ввод
        GlassPanel {
            Layout.fillWidth: true
            padding: AuraTheme.spaceSm
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            RowLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceSm

                GlassField {
                    id: composer
                    Layout.fillWidth: true
                    implicitHeight: 48
                    placeholderText: "Сообщение…"
                    enabled: !(App && App.busy)
                    onAccepted: sendButton.clicked()
                }

                GlassButton {
                    id: sendButton
                    text: ""
                    glyph: "➤"
                    implicitWidth: 52
                    implicitHeight: 48
                    enabled: composer.text.trim().length > 0 && !(App && App.busy)
                    onClicked: {
                        App.sendMessage(page.chatId, composer.text)
                        composer.text = ""
                    }
                }
            }
        }

        // ------------------------------------------------- голосовой ввод
        VoicePanel {
            Layout.fillWidth: true
            backdrop: page.Window.window ? page.Window.window.contentItem : null
        }

        // ------------------------------------------------- запрос к Ауре
        GlassPanel {
            Layout.fillWidth: true
            padding: AuraTheme.spaceMd
            fill: AuraTheme.accentFill * 0.5
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            RowLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceMd

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: AuraTheme.accentA(0.2)
                    border.width: 1
                    border.color: AuraTheme.accentA(0.45)
                    Text {
                        anchors.centerIn: parent
                        text: "✦"
                        color: AuraTheme.accent
                        font.pixelSize: 15
                    }
                }

                GlassField {
                    id: agentField
                    Layout.fillWidth: true
                    implicitHeight: 48
                    placeholderText: "Хочу встретиться с Аней в эти выходные, чтобы обсудить стартап"
                    enabled: !(App && App.busy)
                    onAccepted: agentButton.clicked()
                }

                GlassButton {
                    id: agentButton
                    variant: "accent"
                    text: "Спросить Ауру"
                    glyph: "✦"
                    implicitHeight: 48
                    busy: App ? App.busy : false
                    onClicked: {
                        App.askAgent(page.chatId, agentField.text)
                        agentField.text = ""
                    }
                }
            }
        }
    }
}
