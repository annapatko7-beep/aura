import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  ConfirmationPanel — барьер подтверждения опасных операций (этап 8).

  Аура не исполняет send_message/send_email/book_table без явного разрешения:
  сервер создаёт отложенное действие, панель показывает его и предлагает
  «Подтвердить» (confirmation.approve → сервер исполняет) или «Отклонить»
  (confirmation.deny). Пуста — не занимает места.
*/
GlassPanel {
    id: panel

    property var items: App ? App.confirmations : []
    property bool hasItems: items && items.length > 0

    visible: hasItems
    implicitHeight: visible ? holderColumn.implicitHeight + padding * 2 : 0
    padding: AuraTheme.spaceMd
    fill: 0.10

    ColumnLayout {
        id: holderColumn
        anchors.fill: parent
        spacing: AuraTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: AuraTheme.warning
            }
            Text {
                Layout.fillWidth: true
                text: "Требуется подтверждение · " + (panel.items ? panel.items.length : 0)
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontSmall
                font.weight: Font.DemiBold
                color: AuraTheme.warning
            }
        }

        Repeater {
            model: panel.items

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: AuraTheme.spaceSm

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: modelData.summary && modelData.summary.length > 0
                              ? modelData.summary : modelData.tool
                        elide: Text.ElideRight
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        text: "инструмент: " + modelData.tool
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }

                GlassButton {
                    variant: "accent"
                    text: "Подтвердить"
                    glyph: "✓"
                    implicitHeight: 38
                    onClicked: App.approveConfirmation(modelData.id)
                }
                GlassButton {
                    variant: "danger"
                    text: "Отклонить"
                    glyph: "✕"
                    implicitHeight: 38
                    onClicked: App.denyConfirmation(modelData.id)
                }
            }
        }
    }
}
