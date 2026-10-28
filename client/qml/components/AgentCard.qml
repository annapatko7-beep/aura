import QtQuick
import QtQuick.Layouts
import Selfvoy

/*
  AgentCard — карточка участника переговоров: аватар, имя, «Selfvoy имени»,
  «голосовая волна» (пульсирует, пока говорит этот Selfvoy) и что он раскрывает.
*/
Rectangle {
    id: root
    property string name: ""
    property string selfvoyName: ""
    property bool talking: false
    property bool them: false
    property var facts: []

    implicitWidth: 250
    implicitHeight: column.implicitHeight + 40
    radius: 24
    color: SelfvoyTheme.surface
    border.width: 1
    border.color: root.them ? SelfvoyTheme.border : SelfvoyTheme.accentA(0.35)

    ColumnLayout {
        id: column
        x: 20; y: 24
        width: root.width - 40
        spacing: 4

        SelfvoyAvatar {
            Layout.alignment: Qt.AlignHCenter
            name: root.name
            size: 76
            ring: true
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 10
            text: root.name
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontTitle
            font.weight: Font.Bold
            color: SelfvoyTheme.textPrimary
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root.selfvoyName
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            color: SelfvoyTheme.textSecondary
        }

        // Волна
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 14
            implicitHeight: 74
            radius: 14
            color: SelfvoyTheme.surface2
            Row {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 3
                Repeater {
                    model: 26
                    delegate: Rectangle {
                        required property int index
                        readonly property real base: 0.25 + Math.abs(Math.sin(index * 1.7 + (root.them ? 2 : 0))) * 0.65
                        width: (parent.width - 25 * 3) / 26
                        height: parent.height * (root.talking ? base * pulse.value : base * 0.35)
                        anchors.bottom: parent.bottom
                        radius: 2
                        color: SelfvoyTheme.accentSolid
                        Behavior on height { NumberAnimation { duration: 260 } }
                        QtObject {
                            id: pulse
                            property real value: 1
                            SequentialAnimation on value {
                                running: root.talking
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.45; duration: 300 + (index % 5) * 70 }
                                NumberAnimation { to: 1.0; duration: 300 + (index % 5) * 70 }
                            }
                        }
                    }
                }
            }
        }

        Repeater {
            model: root.facts
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                Icon { name: modelData.icon; size: 13; color: SelfvoyTheme.textSecondary }
                Text {
                    Layout.fillWidth: true
                    text: modelData.text
                    wrapMode: Text.WordWrap
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontMicro + 1
                    color: SelfvoyTheme.textSecondary
                }
            }
        }
    }
}
