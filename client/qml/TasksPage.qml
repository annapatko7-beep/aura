import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  TasksPage — задачи (макет Tasks): неделя, задачи выбранного дня, «Дальше»,
  «Выполнено», подсказка «до следующего события» и FAB «+».
  Данные — tasks.list; отметка — tasks.complete/reopen; новая — tasks.create.
*/
Item {
    id: page

    signal navigate(string to)
    // «Создать → Новая задача» из оболочки.
    function runAction(action) { if (action === "new") addDialog.open() }

    property date now: new Date()
    property date day: new Date()
    readonly property var week: F.weekOf(day)
    readonly property var all: App ? App.tasks : []
    readonly property var open: all.filter(t => t.status === "pending")
                                   .sort((a, b) => (F.taskWhen(a) || "9999").localeCompare(F.taskWhen(b) || "9999"))
    readonly property var dayTasks: open.filter(t => { const d = F.parse(F.taskWhen(t)); return d ? F.sameDay(d, page.day) : F.sameDay(page.day, page.now) })
    readonly property var later: open.filter(t => { const d = F.parse(F.taskWhen(t)); return d && d >= F.addDays(F.startOfDay(page.day), 1) })
    readonly property var done: all.filter(t => t.status === "done").slice(0, 6)
    readonly property var next: open.find(t => { const d = F.parse(F.taskWhen(t)); return d && d > page.now })
    readonly property bool isToday: F.sameDay(day, now)

    Timer { interval: 30000; running: true; repeat: true; onTriggered: page.now = new Date() }

    function tone(task) {
        if (task.status === "done") return SelfvoyTheme.success
        const d = F.parse(F.taskWhen(task))
        if (d && d < page.now) return SelfvoyTheme.danger
        if (d && d - page.now < 2 * 3600000) return SelfvoyTheme.warning
        return SelfvoyTheme.accentSoft
    }

    component Section: ColumnLayout {
        property string title: ""
        property var items: []
        property string empty: ""
        Layout.fillWidth: true
        spacing: 12
        Text {
            text: parent.title
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontTitle
            font.weight: Font.Bold
            color: SelfvoyTheme.text
        }
        Text {
            visible: parent.items.length === 0
            text: parent.empty
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            color: SelfvoyTheme.text3
        }
        Repeater {
            model: parent.items
            delegate: TaskRow { required property var modelData; task: modelData }
        }
    }

    component TaskRow: Rectangle {
        id: row
        property var task: ({})
        readonly property bool isDone: task.status === "done"
        Layout.fillWidth: true
        implicitHeight: 58
        radius: 16
        opacity: isDone ? 0.62 : 1
        color: rowArea.containsMouse ? SelfvoyTheme.surface2 : SelfvoyTheme.surface
        border.width: 1
        border.color: SelfvoyTheme.glass(0.16)

        MouseArea { id: rowArea; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
        Rectangle { x: 8; y: 8; width: 5; height: 5; radius: 2.5; color: SelfvoyTheme.accentSoft }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 14

            // чекбокс
            Rectangle {
                implicitWidth: 22; implicitHeight: 22; radius: 7
                color: row.isDone ? SelfvoyTheme.accent : "transparent"
                border.width: row.isDone ? 0 : 1.5
                border.color: SelfvoyTheme.glass(0.3)
                Icon { anchors.centerIn: parent; visible: row.isDone; name: "check"; size: 14; color: "#FFFFFF" }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: row.isDone ? App.reopenTask(row.task.id) : App.completeTask(row.task.id)
                }
            }
            Text {
                Layout.fillWidth: true
                text: row.task.title
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                font.weight: Font.DemiBold
                font.strikeout: row.isDone
                color: SelfvoyTheme.text
                elide: Text.ElideRight
            }
            Text {
                text: {
                    const when = F.taskWhen(row.task)
                    if (!when) return ""
                    const d = F.parse(when)
                    if (F.sameDay(d, page.day)) return F.time(when)
                    // День недели — только в пределах показанной недели, иначе «11 октября».
                    const inWeek = page.week.some(w => F.sameDay(w, d))
                    return (inWeek ? F.dayShort(d) : F.dateLabel(d)) + ", " + F.time(when)
                }
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                color: SelfvoyTheme.text3
            }
            GlassButton {
                visible: rowArea.containsMouse
                variant: "quiet"
                implicitWidth: 30; implicitHeight: 30
                glyphSize: 14
                symbol: "trash-2"
                onClicked: App.deleteTask(row.task.id)
            }
            Rectangle {
                implicitWidth: 10; implicitHeight: 10; radius: 5
                color: page.tone(row.task)
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: layout.implicitHeight + 120
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: layout
            x: 48
            y: 44
            width: parent.width - 96
            spacing: 30

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Задачи"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: 38
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                GlassButton {
                    variant: "accent"
                    text: "Сегодня"
                    implicitHeight: 44
                    onClicked: page.day = new Date()
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width > 860 ? 2 : 1
                columnSpacing: 40
                rowSpacing: 30

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 14
                    Layout.alignment: Qt.AlignTop
                    spacing: 30

                    // Неделя: стрелки + полоса дней
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 24
                        Rectangle {
                            implicitHeight: 44
                            implicitWidth: weekNav.implicitWidth + 12
                            radius: 22
                            color: SelfvoyTheme.glass(0.05)
                            border.width: 1
                            border.color: SelfvoyTheme.glass(0.14)
                            RowLayout {
                                id: weekNav
                                anchors.centerIn: parent
                                spacing: 4
                                GlassButton { variant: "quiet"; implicitWidth: 32; implicitHeight: 32; glyphSize: 15; symbol: "chevron-left"; onClicked: page.day = F.addDays(page.day, -7) }
                                Text {
                                    text: F.dateLabel(page.week[0]) + " – " + F.dateLabel(page.week[6])
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall
                                    font.weight: Font.DemiBold
                                    color: SelfvoyTheme.text
                                }
                                GlassButton { variant: "quiet"; implicitWidth: 32; implicitHeight: 32; glyphSize: 15; symbol: "chevron-right"; onClicked: page.day = F.addDays(page.day, 7) }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: page.week
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool selected: F.sameDay(modelData, page.day)
                                readonly property bool has: page.open.some(t => { const d = F.parse(F.taskWhen(t)); return d && F.sameDay(d, modelData) })
                                Layout.fillWidth: true
                                implicitHeight: 84
                                radius: 16
                                color: selected ? SelfvoyTheme.accentA(0.2) : (dayArea.containsMouse ? SelfvoyTheme.glass(0.05) : "transparent")
                                border.width: selected ? 1 : 0
                                border.color: SelfvoyTheme.lilacA(0.45)
                                ColumnLayout {
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: F.dayShort(modelData)
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontSmall
                                        color: SelfvoyTheme.text3
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignHCenter
                                        text: modelData.getDate()
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: 26
                                        font.weight: Font.Bold
                                        color: F.sameDay(modelData, page.now) ? SelfvoyTheme.lilac : SelfvoyTheme.text
                                    }
                                    Rectangle {
                                        Layout.alignment: Qt.AlignHCenter
                                        implicitWidth: 5; implicitHeight: 5; radius: 2.5
                                        color: has ? SelfvoyTheme.accentSoft : "transparent"
                                    }
                                }
                                MouseArea { id: dayArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: page.day = modelData }
                            }
                        }
                    }

                    Section {
                        title: page.isToday ? "Сегодня" : F.dayShort(page.day) + ", " + F.dateLabel(page.day)
                        items: page.dayTasks
                        empty: "Свободный день"
                    }
                    Section { title: "Дальше"; items: page.later; empty: "Больше ничего не запланировано" }
                    Section { title: "Выполнено"; items: page.done; empty: "Пока ничего" }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 10
                    Layout.alignment: Qt.AlignTop
                    spacing: 18

                    GlassPanel {
                        Layout.fillWidth: true
                        radius: 24
                        padding: 26
                        backdrop: null
                        base: 0.4
                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: 12
                            Text {
                                text: "Подсказки Selfvoy"
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontLead + 2
                                font.weight: Font.Bold
                                color: SelfvoyTheme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: page.next ? "«" + page.next.title + "» " + F.until(F.taskWhen(page.next))
                                                : "Впереди ничего не запланировано."
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontBody
                                font.weight: Font.DemiBold
                                color: SelfvoyTheme.text
                            }
                            // Прогресс: заполняется, пока до события меньше двух часов
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.topMargin: 10
                                visible: !!page.next
                                implicitHeight: 6
                                radius: 3
                                color: SelfvoyTheme.glass(0.08)
                                Rectangle {
                                    height: parent.height
                                    radius: 3
                                    width: {
                                        if (!page.next) return 0
                                        const min = (F.parse(F.taskWhen(page.next)) - page.now) / 60000
                                        return parent.width * Math.max(0.05, Math.min(1, 1 - min / 120))
                                    }
                                    color: SelfvoyTheme.accentSolid
                                }
                            }
                            Text {
                                visible: !!page.next
                                text: "Время до следующего события"
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontMicro
                                color: SelfvoyTheme.text3
                            }
                        }
                    }

                    GlassPanel {
                        Layout.fillWidth: true
                        radius: 20
                        padding: 20
                        backdrop: null
                        base: 0.3
                        ColumnLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: 12
                            Text {
                                text: "Хотите с кем-то встретиться?"
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                color: SelfvoyTheme.text2
                            }
                            GlassButton {
                                text: "Начать переговоры"
                                symbol: "handshake"
                                onClicked: page.navigate("negotiations")
                            }
                        }
                    }
                }
            }
        }
    }

    // FAB «+»
    GlassButton {
        variant: "accent"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 32
        implicitWidth: 64
        implicitHeight: 64
        glyphSize: 28
        symbol: "plus"
        onClicked: addDialog.open()
    }

    Popup {
        id: addDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 420
        modal: true
        padding: 26
        Overlay.modal: Rectangle { color: SelfvoyTheme.dim }
        onOpened: { title.text = ""; time.text = "18:00"; title.forceActiveFocus() }
        background: GlassSurface { radius: 26; base: 0.97 }

        contentItem: ColumnLayout {
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Новая задача · " + F.dateLabel(page.day)
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontLead
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                GlassButton { variant: "quiet"; implicitWidth: 32; implicitHeight: 32; glyphSize: 14; symbol: "x"; onClicked: addDialog.close() }
            }
            GlassField { id: title; Layout.fillWidth: true; label: "Что сделать"; placeholderText: "Например, отправить питч-дек" }
            GlassField {
                id: time
                Layout.fillWidth: true
                label: "Время (ЧЧ:ММ)"
                inputMask: "99:99"
                onAccepted: add.clicked()
            }
            GlassButton {
                id: add
                Layout.fillWidth: true
                variant: "accent"
                text: "Добавить"
                enabled: title.text.trim().length > 0
                onClicked: {
                    const parts = time.text.split(":")
                    const due = new Date(page.day)
                    due.setHours(parseInt(parts[0]) || 0, parseInt(parts[1]) || 0, 0, 0)
                    App.createTaskAt(title.text, due.toISOString(), "")
                    addDialog.close()
                }
            }
        }
    }
}
