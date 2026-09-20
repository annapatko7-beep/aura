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
    signal openNotifications()

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

                // Этап 13: «входящая» уведомлений (бейдж — непрочитанные).
                Item {
                    implicitWidth: bellButton.implicitWidth
                    implicitHeight: 40

                    GlassButton {
                        id: bellButton
                        variant: "quiet"
                        text: ""
                        glyph: "🔔"
                        implicitWidth: 48
                        implicitHeight: 40
                        onClicked: {
                            if (App) {
                                App.loadNotifications()
                                App.loadPushDevices()
                            }
                            page.openNotifications()
                        }
                    }

                    Rectangle {
                        visible: App && App.unreadNotifications > 0
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 2
                        width: bellBadge.width + 8
                        height: 15
                        radius: AuraTheme.radiusPill
                        color: AuraTheme.accent
                        Text {
                            id: bellBadge
                            anchors.centerIn: parent
                            text: App ? String(App.unreadNotifications) : "0"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: 9
                            font.weight: Font.Bold
                            color: "#04121F"
                        }
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

        // -------------------------------------------- задачи и напоминания
        TasksPanel {
            Layout.fillWidth: true
            backdrop: page.Window.window ? page.Window.window.contentItem : null
        }

        // -------------------------------------------------- список чатов
        // Компонент ChatList (этап 12) — тот же список в desktop-оболочке.
        ChatList {
            Layout.fillWidth: true
            Layout.fillHeight: true
            backdrop: page.Window.window ? page.Window.window.contentItem : null
            onOpenChat: function(chatId) { page.openChat(chatId) }
        }
    }
}
