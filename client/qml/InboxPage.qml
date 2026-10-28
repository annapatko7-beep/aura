import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy

/*
  InboxPage — «Подтверждения» (опасные действия Selfvoy ждут решения) и
  «Уведомления» (входящая + push-устройства). Содержимое — готовые панели
  ConfirmationPanel/NotificationsPanel, страница даёт заголовок и прокрутку.
*/
Item {
    id: page

    signal navigate(string to)
    property string mode: "confirmations"

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 88
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            x: 48
            y: 44
            width: parent.width - 96
            spacing: 22

            Text {
                text: page.mode === "confirmations" ? "Подтверждения" : "Входящие"
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontDisplay
                font.weight: Font.Black
                color: SelfvoyTheme.textPrimary
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: page.mode === "confirmations"
                      ? "Письма, сообщения и брони Selfvoy не выполняет сам: каждое такое действие ждёт здесь вашего решения."
                      : "Записи и оплаты клиентов, «Приду», абонементы, напоминания, переговоры, новые входы и изменения безопасности."
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: SelfvoyTheme.textSecondary
            }
            Loader {
                Layout.fillWidth: true
                sourceComponent: page.mode === "confirmations" ? confirmations : notifications
            }
        }
    }

    Component { id: confirmations; ConfirmationPanel { backdrop: null } }
    Component { id: notifications; NotificationsPanel { backdrop: null } }
}
