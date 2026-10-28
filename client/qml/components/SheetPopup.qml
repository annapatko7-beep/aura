import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy

/*
  SheetPopup — окно с крупными скруглениями (как Sheet в вебе) поверх
  затемнения: заголовок, подзаголовок, кнопка закрытия и прокручиваемое
  содержимое. На десктопе встаёт по центру окна.
*/
Popup {
    id: sheet
    property string title: ""
    property string subtitle: ""
    property Component leading: null
    property int sheetWidth: 560
    default property alias content: body.data

    parent: Overlay.overlay
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(sheetWidth, (parent ? parent.width : sheetWidth) - 48)
    height: Math.min(implicitHeight, (parent ? parent.height : 800) - 48)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    padding: 0

    Overlay.modal: Rectangle { color: SelfvoyTheme.dim }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: SelfvoyTheme.moveMs }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: SelfvoyTheme.moveMs; easing.type: SelfvoyTheme.ease }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: SelfvoyTheme.pressMs }
    }

    background: Rectangle {
        radius: SelfvoyTheme.radiusSheet
        color: SelfvoyTheme.dark ? SelfvoyTheme.surface : SelfvoyTheme.bg
        border.width: SelfvoyTheme.dark ? 1 : 0
        border.color: SelfvoyTheme.border
    }

    implicitHeight: header.implicitHeight + flick.contentHeight + 22 + 16 + 20 + 4

    contentItem: Item {
        RowLayout {
            id: header
            x: 24; y: 22
            width: parent.width - 48
            spacing: 14
            Loader { sourceComponent: sheet.leading; active: sheet.leading !== null; visible: active }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: sheet.title
                    elide: Text.ElideRight
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: 22
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                Text {
                    Layout.fillWidth: true
                    visible: sheet.subtitle.length > 0
                    text: sheet.subtitle
                    elide: Text.ElideRight
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    color: SelfvoyTheme.text2
                }
            }
            GlassButton {
                Layout.alignment: Qt.AlignTop
                variant: "neutral"
                symbol: "x"
                implicitWidth: 32; implicitHeight: 32
                Accessible.name: "Закрыть"
                onClicked: sheet.close()
            }
        }
        Flickable {
            id: flick
            anchors.top: header.bottom
            anchors.topMargin: 16
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.bottomMargin: 20
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: body
                width: flick.width
                spacing: 14
            }
        }
    }
}
