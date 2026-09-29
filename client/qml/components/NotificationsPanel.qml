import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  NotificationsPanel — «входящая» уведомлений и push-устройства (этап 13).

  Список: свежие первыми, непрочитанные подсвечены; клик помечает прочитанным
  (notifications.read). Кнопка «Прочитать все» шлёт id=0. Ниже — push-устройства
  (devices.push.list) с возможностью отозвать токен (devices.push.revoke).
*/
GlassPanel {
    id: panel

    property var items: App ? App.notifications : []
    property var devices: App ? App.pushDevices : []

    implicitHeight: holderColumn.implicitHeight + padding * 2
    padding: AuraTheme.spaceMd
    fill: 0.10

    ColumnLayout {
        id: holderColumn
        anchors.fill: parent
        spacing: AuraTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceSm

            Text {
                Layout.fillWidth: true
                text: "Уведомления · непрочитанных: " + (App ? App.unreadNotifications : 0)
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontSmall
                font.weight: Font.DemiBold
                color: AuraTheme.textPrimary
            }

            GlassButton {
                visible: App && App.unreadNotifications > 0
                text: "Прочитать все"
                glyph: "✓✓"
                implicitHeight: 34
                onClicked: App.markNotificationsRead(0)
            }
        }

        Text {
            Layout.fillWidth: true
            visible: !panel.items || panel.items.length === 0
            text: "Пока тихо. Сюда приходят напоминания о задачах, запросы "
                  + "подтверждения, предложения других агентов и уведомления "
                  + "безопасности (новый вход, 2FA)."
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontSmall
            color: AuraTheme.textMuted
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: panel.items

            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: rowContent.implicitHeight + AuraTheme.spaceSm
                radius: AuraTheme.radiusField
                color: modelData.read ? "transparent" : AuraTheme.accentA(0.08)
                border.width: 1
                border.color: modelData.read ? Qt.rgba(1, 1, 1, AuraTheme.glassBorder)
                                             : AuraTheme.accentA(0.3)

                RowLayout {
                    id: rowContent
                    anchors.fill: parent
                    anchors.margins: AuraTheme.spaceSm
                    spacing: AuraTheme.spaceSm

                    Text {
                        text: panel.glyphFor(modelData.kind)
                        font.pixelSize: 16
                        color: modelData.read ? AuraTheme.textMuted : AuraTheme.accent
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: modelData.title
                            elide: Text.ElideRight
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            font.weight: modelData.read ? Font.Normal : Font.DemiBold
                            color: AuraTheme.textPrimary
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: modelData.body && modelData.body.length > 0
                            text: modelData.body || ""
                            elide: Text.ElideRight
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textSecondary
                        }
                        Text {
                            text: modelData.created_at || ""
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                        }
                    }

                    GlassButton {
                        visible: !modelData.read
                        text: ""
                        glyph: "✓"
                        implicitWidth: 40
                        implicitHeight: 34
                        onClicked: App.markNotificationsRead(modelData.id)
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (!modelData.read) App.markNotificationsRead(modelData.id)
                }
            }
        }

        // -------------------------------------------------- push-устройства
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: AuraTheme.spaceXs
            height: 1
            color: Qt.rgba(1, 1, 1, AuraTheme.glassBorder)
        }

        Text {
            text: "Push-устройства"
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontSmall
            font.weight: Font.DemiBold
            color: AuraTheme.textPrimary
        }

        Text {
            Layout.fillWidth: true
            visible: !panel.devices || panel.devices.length === 0
            text: "Устройств с push-доставкой нет. iOS регистрирует токен APNs "
                  + "автоматически после разрешения на уведомления."
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontMicro
            color: AuraTheme.textMuted
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: panel.devices

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: AuraTheme.spaceSm

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: modelData.platform
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        text: "зарегистрировано: " + (modelData.created_at || "")
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }

                GlassButton {
                    variant: "danger"
                    text: "Отозвать"
                    glyph: "✕"
                    implicitHeight: 34
                    onClicked: App.revokePushDevice(modelData.id)
                }
            }
        }
    }

    // Пиктограмма по виду уведомления (те же kind, что на сервере).
    function glyphFor(kind) {
        switch (kind) {
        case "task.due": return "⏰"
        case "confirmation.requested": return "⚠"
        case "a2a.proposal": return "🤝"
        case "login.new": return "→"
        case "twofactor.enabled": return "🛡"
        case "twofactor.disabled": return "⛨"
        default: return "🔔"
        }
    }
}
