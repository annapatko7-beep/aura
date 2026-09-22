import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  TasksPanel — задачи и напоминания (этап 8).

  Список активных задач (tasks.list), добавление (tasks.create) с необязательным
  сроком напоминания, отметка «выполнено» (tasks.complete) и удаление
  (tasks.delete). Наступившие напоминания сервер пушит событием task.due —
  AppStore добавляет их в начало списка и озвучивает.
*/
GlassPanel {
    id: panel

    property var allTasks: App ? App.tasks : []
    property var activeTasks: (allTasks || []).filter(function (t) { return t.status === "pending" })

    padding: AuraTheme.spaceMd

    ColumnLayout {
        anchors.fill: parent
        spacing: AuraTheme.spaceSm

        // ------------------------------------------------------------ шапка
        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Задачи и напоминания"
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontSmall
                font.weight: Font.DemiBold
                color: AuraTheme.textSecondary
            }
            Item { Layout.fillWidth: true }
            Text {
                text: panel.activeTasks.length + " активных"
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontMicro
                color: AuraTheme.textMuted
            }
        }

        // ------------------------------------------------------- добавление
        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceSm

            GlassField {
                id: titleField
                Layout.fillWidth: true
                implicitHeight: 42
                placeholderText: "новая задача…"
                onAccepted: addButton.clicked()
            }
            GlassField {
                id: remindField
                Layout.preferredWidth: 210
                implicitHeight: 42
                placeholderText: "напомнить: 2026-09-21 10:00"
                onAccepted: addButton.clicked()
            }
            GlassButton {
                id: addButton
                variant: "accent"
                text: ""
                glyph: "＋"
                implicitWidth: 46
                implicitHeight: 42
                enabled: titleField.text.trim().length > 0
                onClicked: {
                    // Сервер ждёт ISO-метку: заменяем пробел на «T».
                    var remind = remindField.text.trim().replace(" ", "T")
                    App.createTask(titleField.text, "", remind)
                    titleField.text = ""
                    remindField.text = ""
                }
            }
        }

        // ----------------------------------------------------------- список
        ListView {
            id: taskList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, 168)
            model: panel.activeTasks
            clip: true
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds

            Text {
                anchors.centerIn: parent
                visible: taskList.count === 0
                text: "Пока пусто — Аура создаёт задачи из напоминаний"
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontMicro
                color: AuraTheme.textMuted
            }

            delegate: RowLayout {
                required property var modelData
                width: taskList.width
                spacing: AuraTheme.spaceSm

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: modelData.remind_at ? AuraTheme.warning : AuraTheme.accent
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: modelData.title
                        elide: Text.ElideRight
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        visible: modelData.remind_at && modelData.remind_at.length > 0
                        text: "напомнить: " + (modelData.remind_at || "").replace("T", " ").substring(0, 16)
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }

                GlassButton {
                    variant: "quiet"
                    text: ""
                    glyph: "✓"
                    implicitWidth: 40
                    implicitHeight: 34
                    onClicked: App.completeTask(modelData.id)
                }
                GlassButton {
                    variant: "quiet"
                    text: ""
                    glyph: "🗑"
                    implicitWidth: 40
                    implicitHeight: 34
                    onClicked: App.deleteTask(modelData.id)
                }
            }
        }
    }
}
