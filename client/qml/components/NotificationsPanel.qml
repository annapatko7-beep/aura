import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "../Format.js" as F

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
    padding: SelfvoyTheme.spaceMd
    fill: 0.10

    ColumnLayout {
        id: holderColumn
        anchors.fill: parent
        spacing: SelfvoyTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: SelfvoyTheme.spaceSm

            Text {
                Layout.fillWidth: true
                text: "Уведомления · непрочитанных: " + (App ? App.unreadNotifications : 0)
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                font.weight: Font.DemiBold
                color: SelfvoyTheme.textPrimary
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
            text: "Пока тихо. Сюда приходят записи и оплаты клиентов, «Приду», "
                  + "абонементы, напоминания о задачах, запросы подтверждения, "
                  + "предложения других агентов и уведомления безопасности."
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            color: SelfvoyTheme.textMuted
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: panel.items

            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: rowContent.implicitHeight + SelfvoyTheme.spaceSm
                radius: SelfvoyTheme.radiusField
                color: modelData.read ? "transparent" : SelfvoyTheme.newSoft
                border.width: modelData.read ? 1 : 0
                border.color: SelfvoyTheme.border

                RowLayout {
                    id: rowContent
                    anchors.fill: parent
                    anchors.margins: SelfvoyTheme.spaceSm
                    spacing: SelfvoyTheme.spaceSm

                    // Вид уведомления — значок на тоне статуса (новое — янтарное).
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: 32; implicitHeight: 32; radius: 10
                        readonly property string tone: F.notificationTone(modelData)
                        color: SelfvoyTheme.toneBg(tone)
                        Icon {
                            anchors.centerIn: parent
                            name: F.notificationIcon(modelData.kind)
                            size: 17
                            color: parent.tone === "new" ? SelfvoyTheme.warning : SelfvoyTheme.toneFg(parent.tone)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: modelData.title
                            elide: Text.ElideRight
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            font.weight: modelData.read ? Font.Normal : Font.DemiBold
                            color: SelfvoyTheme.textPrimary
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: F.notificationDetail(modelData)
                            elide: Text.ElideRight
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontMicro
                            color: SelfvoyTheme.textSecondary
                        }
                        Text {
                            text: F.stamp(modelData.created_at)
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontMicro
                            color: SelfvoyTheme.textMuted
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
            Layout.topMargin: SelfvoyTheme.spaceXs
            height: 1
            color: SelfvoyTheme.border
        }

        Text {
            text: "Push-устройства"
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            font.weight: Font.DemiBold
            color: SelfvoyTheme.textPrimary
        }

        Text {
            Layout.fillWidth: true
            visible: !panel.devices || panel.devices.length === 0
            text: "Устройств с push-доставкой нет. iOS регистрирует токен APNs "
                  + "автоматически после разрешения на уведомления."
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontMicro
            color: SelfvoyTheme.textMuted
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: panel.devices

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: SelfvoyTheme.spaceSm

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: modelData.platform
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.textPrimary
                    }
                    Text {
                        text: "зарегистрировано: " + F.stamp(modelData.created_at)
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontMicro
                        color: SelfvoyTheme.textMuted
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
}
