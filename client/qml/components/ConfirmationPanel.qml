import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "../Format.js" as F

/*
  ConfirmationPanel — барьер подтверждения опасных операций (этап 8).

  Selfvoy не исполняет send_message/send_email/book_table без явного разрешения:
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
    padding: SelfvoyTheme.spaceMd
    fill: 0.10

    ColumnLayout {
        id: holderColumn
        anchors.fill: parent
        spacing: SelfvoyTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: SelfvoyTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: SelfvoyTheme.warning
            }
            Text {
                Layout.fillWidth: true
                text: "Требуется подтверждение · " + (panel.items ? panel.items.length : 0)
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                font.weight: Font.DemiBold
                color: SelfvoyTheme.warning
            }
        }

        Repeater {
            model: panel.items

            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: SelfvoyTheme.spaceSm

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: modelData.summary && modelData.summary.length > 0
                              ? modelData.summary : F.toolDescription(modelData.tool, modelData.tool)
                        elide: Text.ElideRight
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.textPrimary
                    }
                    Text {
                        text: F.toolTitle(modelData.tool) + (modelData.created_at ? " · " + F.stamp(modelData.created_at) : "")
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontMicro
                        color: SelfvoyTheme.textMuted
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
