import QtQuick
import QtQuick.Layouts
import Selfvoy

/*
  Bubble — реплика в диалоге с Selfvoy. Пользователь — мягкий акцентный фон,
  Selfvoy — сплошная карточка (дизайн 2026, без стекла и свечения).
  Дети (кнопки действий) встают под текстом.
*/
Item {
    id: root
    property bool fromSelfvoy: false
    property bool error: false
    property string text: ""
    property real maxWidth: 560
    // Под репликой есть кнопки действий — пузырь берёт полную ширину, кнопки переносятся.
    property bool wide: false
    default property alias extra: below.data

    // Ширину меряем копией текста без переноса: implicitWidth переносимого Text
    // внутри Layout занижен, и короткие реплики переносились на две строки.
    implicitWidth: wide ? maxWidth : Math.min(maxWidth, Math.ceil(probe.implicitWidth) + 2 + 44)
    implicitHeight: column.implicitHeight + 36
    width: implicitWidth
    height: implicitHeight

    Text {
        id: probe
        visible: false
        text: root.text
        font: label.font
    }

    Rectangle {
        anchors.fill: parent
        radius: SelfvoyTheme.radiusBubble
        color: root.fromSelfvoy ? SelfvoyTheme.surface : SelfvoyTheme.accentTint
        border.width: root.error ? 1 : 0
        border.color: SelfvoyTheme.danger
    }
    ColumnLayout {
        id: column
        x: 22
        y: 18
        width: root.width - 44
        spacing: 14
        Text {
            id: label
            Layout.fillWidth: true
            text: root.text
            wrapMode: Text.WordWrap
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontBody
            font.weight: Font.Normal
            lineHeight: 1.3
            color: root.error ? SelfvoyTheme.danger : SelfvoyTheme.textPrimary
        }
        Item {
            id: below
            Layout.fillWidth: true
            implicitHeight: childrenRect.height
            visible: root.wide
        }
    }
}
