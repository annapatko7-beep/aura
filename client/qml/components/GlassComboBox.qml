import QtQuick
import QtQuick.Controls
import Selfvoy

// GlassComboBox — выпадающий список в стиле полей 2026 (серая подложка, акцентный фокус).
ComboBox {
    id: control
    implicitHeight: 44
    implicitWidth: 180
    font.family: SelfvoyTheme.fontFamily
    font.pixelSize: SelfvoyTheme.fontSmall + 1

    background: Rectangle {
        radius: SelfvoyTheme.radiusField
        color: control.pressed ? SelfvoyTheme.fieldHover : SelfvoyTheme.field
        border.width: control.visualFocus || control.popup.visible ? 1 : 0
        border.color: SelfvoyTheme.accent
    }
    contentItem: Text {
        leftPadding: 14
        rightPadding: 34
        text: control.displayText
        font: control.font
        color: SelfvoyTheme.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Icon {
        x: control.width - width - 12
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        size: 16
    }
    delegate: ItemDelegate {
        id: option
        required property int index
        required property var modelData
        width: control.width
        height: 38
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textRole ? (Array.isArray(control.model) ? option.modelData[control.textRole] : option.modelData)
                                   : option.modelData
            font: control.font
            color: SelfvoyTheme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 10
            color: option.highlighted ? SelfvoyTheme.accentTint : "transparent"
        }
    }
    popup: Popup {
        y: control.height + 6
        width: control.width
        padding: 6
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 320)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
        }
        background: Rectangle {
            radius: SelfvoyTheme.radiusField
            color: SelfvoyTheme.bgElevated
            border.width: 1
            border.color: SelfvoyTheme.border
        }
    }
}
