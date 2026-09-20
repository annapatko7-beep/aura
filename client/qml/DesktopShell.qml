import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  DesktopShell — широкая раскладка (этап 12).

  Навигационный рельс слева (Чаты / Задачи / Подтверждения / Настройки) и
  контент справа. Чаты — master-detail: список в левой колонке, диалог
  справа, без стековой навигации. Включается из Main.qml при ширине окна
  >= AuraTheme.breakpointWide; на узком окне остаётся стек ChatsPage → ChatPage.
*/
Item {
    id: shell

    // Выбранный чат (общий с Main.qml — переживает смену раскладки).
    property int selectedChatId: 0
    property int currentTab: 0

    signal chatSelected(int chatId)

    readonly property var backdropRef: shell.Window.window ? shell.Window.window.contentItem : null

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------- рельс навигации
        GlassPanel {
            Layout.preferredWidth: AuraTheme.railWidth
            Layout.fillHeight: true
            Layout.margins: AuraTheme.spaceMd
            Layout.rightMargin: AuraTheme.spaceSm
            padding: AuraTheme.spaceSm
            backdrop: shell.backdropRef

            ColumnLayout {
                anchors.fill: parent
                spacing: AuraTheme.spaceSm

                Repeater {
                    model: [
                        { glyph: "💬", label: "Чаты" },
                        { glyph: "✓", label: "Задачи" },
                        { glyph: "⚠", label: "Ждут" },
                        { glyph: "⚙", label: "Ещё" }
                    ]

                    delegate: Item {
                        id: railItem
                        required property int index
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 58

                        readonly property bool active: shell.currentTab === index

                        Rectangle {
                            anchors.fill: parent
                            radius: AuraTheme.radiusField
                            color: railItem.active ? AuraTheme.accentA(0.18)
                                                   : (railArea.containsMouse ? Qt.rgba(1, 1, 1, 0.08)
                                                                             : "transparent")
                            border.width: 1
                            border.color: railItem.active ? AuraTheme.accentA(0.45) : "transparent"
                            Behavior on color { ColorAnimation { duration: AuraTheme.pressMs } }

                            ColumnLayout {
                                anchors.centerIn: parent
                                spacing: 2
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: railItem.modelData.glyph
                                    font.pixelSize: 18
                                    color: railItem.active ? AuraTheme.accent : AuraTheme.textSecondary
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: railItem.modelData.label
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: 9
                                    color: railItem.active ? AuraTheme.accent : AuraTheme.textMuted
                                }
                            }

                            // Бейдж неподтверждённых опасных операций
                            Rectangle {
                                visible: railItem.index === 2 && App && App.confirmations.length > 0
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 3
                                width: badgeText.width + 10
                                height: 16
                                radius: AuraTheme.radiusPill
                                color: AuraTheme.warning
                                Text {
                                    id: badgeText
                                    anchors.centerIn: parent
                                    text: App ? String(App.confirmations.length) : "0"
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: 9
                                    font.weight: Font.Bold
                                    color: "#1A1400"
                                }
                            }
                        }

                        MouseArea {
                            id: railArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                shell.currentTab = railItem.index
                                if (railItem.index === 1 && App) App.loadTasks()
                                if (railItem.index === 2 && App) App.loadConfirmations()
                                if (railItem.index === 3 && App) {
                                    App.loadPermissions()
                                    App.loadIntegrations()
                                }
                            }
                        }

                        ToolTip.visible: railArea.containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: railItem.index === 2 ? "Подтверждения"
                                      : (railItem.index === 3 ? "Настройки" : railItem.modelData.label)
                    }
                }

                Item { Layout.fillHeight: true }

                AuraAvatar {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.bottomMargin: AuraTheme.spaceXs
                    name: App ? App.userName : ""
                    online: App ? App.connected : false
                    size: 40
                }
            }
        }

        // ------------------------------------------------------- контент
        StackLayout {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: shell.currentTab

            // 0 — Чаты: master-detail
            RowLayout {
                spacing: 0

                ColumnLayout {
                    Layout.preferredWidth: AuraTheme.masterWidth
                    Layout.fillHeight: true
                    Layout.margins: AuraTheme.spaceMd
                    Layout.rightMargin: AuraTheme.spaceSm
                    spacing: AuraTheme.spaceMd

                    // Новый собеседник
                    GlassPanel {
                        Layout.fillWidth: true
                        padding: AuraTheme.spaceMd
                        backdrop: shell.backdropRef

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
                                text: ""
                                glyph: "＋"
                                implicitWidth: 52
                                implicitHeight: 46
                                busy: App ? App.busy : false
                                onClicked: {
                                    App.openChat(contactField.text)
                                    contactField.text = ""
                                }
                            }
                        }
                    }

                    ChatList {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        selectedChatId: shell.selectedChatId
                        backdrop: shell.backdropRef
                        // Только сигнал: selectedChatId придёт обратно биндингом
                        // из Main.qml (императивное присваивание сломало бы его).
                        onOpenChat: function(chatId) { shell.chatSelected(chatId) }
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: AuraTheme.spaceMd
                    Layout.bottomMargin: AuraTheme.spaceMd
                    Layout.rightMargin: AuraTheme.spaceMd
                    sourceComponent: shell.selectedChatId > 0 ? chatDetail : chatPlaceholder
                }
            }

            // 1 — Задачи
            Item {
                Flickable {
                    anchors.fill: parent
                    anchors.margins: AuraTheme.spaceLg
                    contentHeight: tasksColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: tasksColumn
                        width: parent.width
                        spacing: AuraTheme.spaceMd

                        Text {
                            text: "Задачи и напоминания"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontTitle
                            font.weight: Font.DemiBold
                            color: AuraTheme.textPrimary
                        }

                        TasksPanel {
                            Layout.fillWidth: true
                            backdrop: shell.backdropRef
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "Напоминания приходят событием task.due — даже когда "
                                  + "открыт другой раздел."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // 2 — Подтверждения опасных операций
            Item {
                Flickable {
                    anchors.fill: parent
                    anchors.margins: AuraTheme.spaceLg
                    contentHeight: confirmColumn.height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    ColumnLayout {
                        id: confirmColumn
                        width: parent.width
                        spacing: AuraTheme.spaceMd

                        Text {
                            text: "Подтверждения"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontTitle
                            font.weight: Font.DemiBold
                            color: AuraTheme.textPrimary
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: App && App.confirmations.length === 0
                            text: "Нет операций, ожидающих подтверждения. Опасные действия "
                                  + "(сообщения, письма, бронирование) Аура выполняет только "
                                  + "после вашего «Разрешить»."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textMuted
                            wrapMode: Text.WordWrap
                        }

                        ConfirmationPanel {
                            Layout.fillWidth: true
                            backdrop: shell.backdropRef
                        }
                    }
                }
            }

            // 3 — Настройки
            SettingsPage {
                embedded: true
            }
        }
    }

    Component {
        id: chatDetail
        ChatPage {
            embedded: true
            chatId: shell.selectedChatId
        }
    }

    Component {
        id: chatPlaceholder
        GlassPanel {
            backdrop: shell.backdropRef

            ColumnLayout {
                anchors.centerIn: parent
                spacing: AuraTheme.spaceSm

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "✦"
                    font.pixelSize: 34
                    color: AuraTheme.accent
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Выберите чат слева\nили начните новый по email"
                    horizontalAlignment: Text.AlignHCenter
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textMuted
                    lineHeight: 1.4
                }
            }
        }
    }
}
