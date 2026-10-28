import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  MySelfvoyPage — диалог с Selfvoy (макет Chat). Реплики уходят в agent.ask без
  привязки к чату; ответ Selfvoy приходит с действиями. Опасные действия
  (письмо, сообщение, бронь) не исполняются, а ждут решения: кнопки
  «Подтвердить / Отклонить» под репликой вызывают confirmation.approve/deny.
*/
Item {
    id: page

    signal navigate(string to)

    readonly property Item backdropRef: page.Window.window ? page.Window.window.contentItem : null
    readonly property var dialog: App ? App.selfvoyDialog : []
    // id подтверждений, которые ещё ждут решения (остальные уже обработаны)
    readonly property var pendingIds: (App ? App.confirmations : []).map(c => c.id)
    property bool showMemory: false

    function send(text) {
        const t = text.trim()
        if (!t || !App || App.busy) return
        App.askAgentGlobal(t)
        input.text = ""
    }

    Connections {
        target: App
        function onTranscriptChanged() { if (App.transcript) input.text = App.transcript }
        function onSelfvoyDialogChanged() { Qt.callLater(() => list.positionViewAtEnd()) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ----------------------------------------------------------------- шапка
        Item {
            Layout.fillWidth: true
            implicitHeight: 118

            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: 33
                color: "transparent"
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 34
                anchors.rightMargin: 34
                spacing: 18
                SelfvoyOrb { size: 66 }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    Text {
                        Layout.fillWidth: true
                        text: "Selfvoy"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: 26
                        font.weight: Font.Bold
                        color: SelfvoyTheme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: (App && App.busy ? "Думает" : App && App.connected ? "В сети" : "Не в сети") + " · Персональный ассистент"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text2
                    }
                }
                GlassButton {
                    text: "Память"
                    symbol: "brain"
                    implicitHeight: 44
                    onClicked: { page.showMemory = !page.showMemory; if (page.showMemory && App) App.refreshMemory() }
                }
            }
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: 1
                color: SelfvoyTheme.glass(0.08)
            }
        }

        // ------------------------------------------------------------- сообщения
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 16
            topMargin: 28
            bottomMargin: 12
            leftMargin: 36
            rightMargin: 36
            model: page.dialog
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            header: Item {
                width: list.width - 72
                height: page.dialog.length === 0 ? welcome.implicitHeight + 24 : 0
                visible: page.dialog.length === 0
                ColumnLayout {
                    id: welcome
                    anchors.right: parent.right
                    width: Math.min(parent.width, 560)
                    spacing: 14
                    Bubble {
                        Layout.alignment: Qt.AlignRight
                        fromSelfvoy: true
                        text: "Привет, " + F.firstName(App ? App.userName : "") + "! Я ваш Selfvoy. Скажите, что нужно сделать, "
                              + "и я договорюсь с Selfvoy других людей, поставлю напоминание или подготовлю письмо."
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        layoutDirection: Qt.RightToLeft
                        Repeater {
                            model: ["Хочу встретиться с Димой в эти выходные и обсудить стартап",
                                    "Что у меня сегодня в расписании?",
                                    "Напомни завтра в 10 позвонить юристу"]
                            delegate: Chip {
                                required property string modelData
                                text: modelData
                                onClicked: page.send(modelData)
                            }
                        }
                    }
                }
            }

            delegate: Item {
                id: row
                required property var modelData
                width: list.width - 72
                height: bubble.implicitHeight

                Bubble {
                    id: bubble
                    fromSelfvoy: row.modelData.from === "selfvoy"
                    error: !!row.modelData.error
                    text: row.modelData.text
                    anchors.right: fromSelfvoy ? parent.right : undefined
                    anchors.left: fromSelfvoy ? undefined : parent.left
                    maxWidth: Math.min(580, row.width * 0.85)
                    wide: (row.modelData.actions && row.modelData.actions.length > 0) || !!row.modelData.a2a

                    // Действия Selfvoy под репликой
                    Flow {
                        width: parent.width
                        spacing: 8
                        visible: (row.modelData.actions && row.modelData.actions.length > 0) || !!row.modelData.a2a
                        Repeater {
                            model: row.modelData.actions || []
                            delegate: Flow {
                                required property var modelData
                                readonly property bool open: page.pendingIds.indexOf(modelData.id) >= 0
                                width: parent.width
                                spacing: 8
                                Text {
                                    width: parent.width
                                    text: (open ? "Ждёт вашего решения: " : "Решено: ") + modelData.summary
                                    wrapMode: Text.WordWrap
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall
                                    color: open ? SelfvoyTheme.warning : SelfvoyTheme.text3
                                }
                                Chip {
                                    visible: open
                                    text: "Подтвердить"
                                    onClicked: App.approveConfirmation(modelData.id)
                                }
                                Chip {
                                    visible: open
                                    text: "Отклонить"
                                    onClicked: App.denyConfirmation(modelData.id)
                                }
                            }
                        }
                        Chip {
                            visible: !!row.modelData.a2a
                            text: "Открыть переговоры"
                            onClicked: page.navigate("negotiations")
                        }
                    }
                }
            }

            footer: Item {
                width: list.width - 72
                height: App && App.busy ? 40 : 0
                visible: App && App.busy
                Row {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    spacing: 8
                    Text {
                        text: "Selfvoy думает"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text2
                    }
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            required property int index
                            width: 5; height: 5; radius: 2.5
                            anchors.verticalCenter: parent.verticalCenter
                            color: SelfvoyTheme.text2
                            SequentialAnimation on opacity {
                                loops: Animation.Infinite
                                PauseAnimation { duration: index * 200 }
                                NumberAnimation { to: 0.2; duration: 500 }
                                NumberAnimation { to: 1.0; duration: 500 }
                            }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------- поле ввода
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 36
            Layout.rightMargin: 36
            Layout.bottomMargin: 26
            Layout.topMargin: 10
            spacing: 14

            GlassButton {
                implicitWidth: 52; implicitHeight: 52
                glyphSize: 20
                symbol: App && App.voiceState === "listening" ? "square" : "mic"
                busy: App ? App.voiceState === "processing" : false
                enabled: App ? App.micAvailable : false
                ToolTip.visible: hovered
                ToolTip.text: App && App.micAvailable ? "Сказать голосом" : "Микрофон не найден"
                onClicked: App.voiceState === "listening" ? App.stopListening() : App.startListening()
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 56
                radius: 28
                color: SelfvoyTheme.surface
                border.width: 1
                border.color: input.activeFocus ? SelfvoyTheme.accentA(0.7) : SelfvoyTheme.glass(0.18)
                TextField {
                    id: input
                    anchors.fill: parent
                    anchors.leftMargin: 22
                    anchors.rightMargin: 22
                    background: null
                    placeholderText: "Напишите что-нибудь…"
                    placeholderTextColor: SelfvoyTheme.text3
                    color: SelfvoyTheme.text
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    verticalAlignment: TextInput.AlignVCenter
                    onAccepted: page.send(text)
                }
            }

            GlassButton {
                variant: "accent"
                implicitWidth: 56; implicitHeight: 56
                glyphSize: 22
                symbol: "send-horizontal"
                enabled: input.text.trim().length > 0 && App && !App.busy
                onClicked: page.send(input.text)
            }
        }
    }

    // ------------------------------------------------------------ память
    GlassPanel {
        visible: page.showMemory
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 24
        anchors.topMargin: 104
        width: 320
        height: Math.min(page.height - 140, implicitHeight)
        radius: 22
        padding: 20
        base: 0.97
        backdrop: null
        z: 10

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: "Что Selfvoy знает о вас"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                GlassButton { implicitWidth: 30; implicitHeight: 30; glyphSize: 14; symbol: "x"; onClicked: page.showMemory = false }
            }
            Repeater {
                model: [
                    { k: "Город", v: App && App.preferences.city ? App.preferences.city : "—" },
                    { k: "Питание", v: App && App.preferences.diet && App.preferences.diet.length ? App.preferences.diet.join(", ") : "без ограничений" },
                    { k: "Бюджет на встречу", v: App && App.preferences.budget_limit ? "до " + App.preferences.budget_limit : "не задан" }
                ]
                delegate: ColumnLayout {
                    required property var modelData
                    spacing: 1
                    Text { text: modelData.k; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontMicro; color: SelfvoyTheme.text3 }
                    Text { text: modelData.v; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontSmall; color: SelfvoyTheme.text }
                }
            }
            Text {
                text: "Факты из разговоров"
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontMicro
                color: SelfvoyTheme.text3
            }
            Repeater {
                model: App ? App.memory.slice(0, 6) : []
                delegate: Text {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "• " + modelData.text
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall
                    color: SelfvoyTheme.text2
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Другие Selfvoy видят только, свободны ли вы в предложенное время, — не ваш календарь и не эти записи."
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontMicro
                color: SelfvoyTheme.text3
            }
        }
    }
}
