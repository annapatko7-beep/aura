import QtQuick
import QtQuick.Layouts
import Selfvoy

/*
  Cell — ячейка сгруппированного списка: значок (необязательно), подпись с
  пояснением и элемент справа (переключатель, поле, значение). divider — тонкая
  линия сверху (у всех ячеек, кроме первой). clickable — вся ячейка кнопка.
*/
Item {
    id: root
    property string label: ""
    property string hint: ""
    property string icon: ""
    property color iconColor: SelfvoyTheme.accentSolid
    property string value: ""
    property bool divider: true
    property bool clickable: false
    property bool stacked: false        // элемент — под подписью на всю ширину
    property bool pro: false            // метка PRO у подписи
    property bool interactive: false     // не кнопка целиком, но внутри trailing — переключатель/поле
    signal clicked()
    default property alias trailing: slot.data

    Layout.fillWidth: true
    implicitHeight: Math.max(48, layout.implicitHeight + 20)

    Rectangle {
        anchors.fill: parent
        color: root.clickable && hit.containsMouse ? SelfvoyTheme.field
               : root.interactive && hit.containsMouse ? SelfvoyTheme.glass(0.03) : "transparent"
        Behavior on color { ColorAnimation { duration: SelfvoyTheme.pressMs } }
    }
    Rectangle {
        visible: root.divider
        x: root.icon.length > 0 ? 58 : 16
        width: parent.width - x
        height: 1
        color: SelfvoyTheme.border
    }
    // Для interactive (переключатель/поле в trailing) — только подсветка-подсказка
    // «здесь есть управление», клик не перехватывается: сам элемент в trailing
    // остаётся источником действия, hit.clicked сюда не ведёт.
    MouseArea {
        id: hit
        anchors.fill: parent
        // enabled остаётся true и для interactive — иначе Qt Quick не шлёт hover
        // при enabled: false. acceptedButtons: пусто — клики не перехватываются,
        // проходят к Toggle/полю в trailing, которые лежат выше в GridLayout.
        enabled: root.clickable || root.interactive
        acceptedButtons: root.clickable ? Qt.LeftButton : Qt.NoButton
        hoverEnabled: root.clickable || root.interactive
        cursorShape: root.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.clicked()
    }

    GridLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        columns: root.stacked ? 1 : 3
        columnSpacing: 12
        rowSpacing: 8

        Rectangle {
            visible: root.icon.length > 0 && !root.stacked
            implicitWidth: 30; implicitHeight: 30; radius: 8
            color: root.iconColor
            Icon { anchors.centerIn: parent; name: root.icon; size: 17; color: "#FFFFFF" }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            visible: root.label.length > 0 || root.hint.length > 0
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: root.label.length > 0
                Text {
                    text: root.label
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    color: SelfvoyTheme.text
                }
                ProBadge { visible: root.pro }
                Item { Layout.fillWidth: true }   // подпись тянется — элемент справа прижат к краю
            }
            Text {
                Layout.fillWidth: true
                visible: root.hint.length > 0
                text: root.hint
                wrapMode: Text.WordWrap
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                color: SelfvoyTheme.text2
            }
        }
        // Справа — в строку; у stacked — столбиком на всю ширину.
        GridLayout {
            id: slot
            Layout.fillWidth: root.stacked
            Layout.alignment: root.stacked ? Qt.AlignLeft : (Qt.AlignRight | Qt.AlignVCenter)
            columns: root.stacked ? 1 : 99
            columnSpacing: 8
            rowSpacing: 8
            Text {
                visible: root.value.length > 0
                text: root.value
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: SelfvoyTheme.text2
            }
            Icon {
                visible: root.clickable
                name: "chevron-right"
                size: 16
                color: SelfvoyTheme.text3
            }
        }
    }
}
