import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  ChatsPage — список чатов. Никакой ленты: только ваши диалоги и Аура.
*/
Item {
    id: page

    signal openChat(int chatId)
    signal openSettings()

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

                AuraAvatar {
                    name: App ? App.userName : ""
                    online: App ? App.connected : false
                    size: 40
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: App ? App.userName : "Гость"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontTitle
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        text: App && App.connected ? "Аура на связи · " + App.userEmail : "нет соединения с сервером"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: App && App.connected ? AuraTheme.textSecondary : AuraTheme.warning
                    }
                }

                GlassButton {
                    variant: "quiet"
                    text: "Настройки"
                    glyph: "⚙"
                    implicitHeight: 40
                    onClicked: page.openSettings()
                }
            }
        }

        // ---------------------------------------------- новый собеседник
        GlassPanel {
            Layout.fillWidth: true
            padding: AuraTheme.spaceMd
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            RowLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceSm

                GlassField {
                    id: contactField
                    Layout.fillWidth: true
                    implicitHeight: 46
                    placeholderText: "email собеседника"
                    onAccepted: startChatButton.clicked()
                }

                GlassButton {
                    id: startChatButton
                    variant: "accent"
                    text: "Начать чат"
                    glyph: "＋"
                    implicitHeight: 46
                    busy: App ? App.busy : false
                    onClicked: {
                        App.openChat(contactField.text)
                        contactField.text = ""
                    }
                }
            }
        }

        // -------------------------------------------------- список чатов
        GlassPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: AuraTheme.spaceMd
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            ColumnLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Чаты"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        font.weight: Font.DemiBold
                        color: AuraTheme.textSecondary
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: (App ? App.chats.length : 0) + " активных"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }

                ListView {
                    id: chatList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: AuraTheme.spaceSm
                    model: App ? App.chats : []

                    // Пустое состояние
                    Text {
                        anchors.centerIn: parent
                        visible: chatList.count === 0
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "Пока пусто.\nДобавьте собеседника по email — ваши Ауры познакомятся сами."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textMuted
                        lineHeight: 1.4
                    }

                    delegate: Rectangle {
                        id: row
                        width: chatList.width
                        height: 72
                        radius: AuraTheme.radiusField
                        color: itemArea.containsMouse ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(1, 1, 1, 0.05)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, itemArea.containsMouse ? 0.18 : AuraTheme.glassBorder)
                        scale: itemArea.pressed ? AuraTheme.pressScale : 1
                        Behavior on scale { NumberAnimation { duration: AuraTheme.pressMs } }
                        Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }

                        readonly property var chat: modelData

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: AuraTheme.spaceMd
                            anchors.rightMargin: AuraTheme.spaceMd
                            spacing: AuraTheme.spaceMd

                            AuraAvatar {
                                name: row.chat ? String(row.chat.title || row.chat.last_message_sender || "A") : "A"
                                online: false
                                size: 42
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: row.chat ? String(row.chat.title || "Без названия") : ""
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontBody
                                    font.weight: Font.DemiBold
                                    color: AuraTheme.textPrimary
                                }
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: row.chat ? String(row.chat.last_message_body || "Сообщений пока нет") : ""
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontSmall
                                    color: AuraTheme.textSecondary
                                }
                            }

                            ColumnLayout {
                                spacing: 4
                                Text {
                                    Layout.alignment: Qt.AlignRight
                                    text: row.chat ? String(row.chat.last_message_at || "").substring(5, 16) : ""
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontMicro
                                    color: AuraTheme.textMuted
                                }
                                Rectangle {
                                    Layout.alignment: Qt.AlignRight
                                    width: agentLabel.width + 12
                                    height: 18
                                    radius: AuraTheme.radiusPill
                                    color: AuraTheme.accentA(0.18)
                                    border.width: 1
                                    border.color: AuraTheme.accentA(0.35)
                                    Text {
                                        id: agentLabel
                                        anchors.centerIn: parent
                                        text: "A2A"
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: 9
                                        font.weight: Font.DemiBold
                                        color: AuraTheme.accent
                                    }
                                }
                            }
                        }

                        MouseArea {
                            id: itemArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.openChat(row.chat ? row.chat.id : 0)
                        }
                    }
                }
            }
        }
    }
}
