import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  NegotiationsPage — переговоры Selfvoy (макет Meeting arrangement).

  Один вызов agent.negotiate = один раунд «предложение → ответ». Шаги в центре
  строятся из ответа сервера: окно моего Selfvoy, проверка календаря собеседника,
  место (или встречные варианты), принятие. Итог бронируется только по кнопке
  пользователя (book_table, режим «запретить» уважается). После 5 раундов —
  эскалация: решает человек (правило A2A_PROTOCOL.md).
*/
Item {
    id: page

    signal navigate(string to)

    readonly property int maxRounds: 5
    readonly property var result: App ? App.negotiation : ({})
    readonly property bool hasResult: result && result.target !== undefined
    readonly property bool accepted: hasResult && !!result.accepted
    readonly property bool booked: hasResult && !!result.booking
    readonly property int round: hasResult ? (result.round || 1) : 0
    readonly property var contacts: F.contactsFrom(App ? App.chats : [], App ? App.userId : 0)
    readonly property string myName: F.firstName(App ? App.userName : "")
    readonly property string theirName: hasResult ? F.firstName(F.nameFor(result.target, App ? App.chats : [], App ? App.userId : 0)) : ""
    property bool composing: !hasResult
    property int revealed: 0

    // Шаги текущего раунда — из данных ответа.
    readonly property var steps: {
        if (!hasResult) return []
        const me = "Selfvoy " + F.genitive(myName)
        const them = "Selfvoy " + F.genitive(theirName)
        const hours = result.window_hours || 96
        const out = [{ who: "me", text: me + " предложил встречу: " + (result.duration_minutes || 60) + " мин",
                       detail: "окно — " + F.windowPhrase(hours)
                               + (result.topic ? " · " + result.topic : "") }]
        if (accepted) {
            out.push({ who: "them", text: them + " проверил календарь " + F.genitive(theirName),
                       detail: "общее окно: " + F.dayPhrase(result.slot.start, true) + " в " + F.time(result.slot.start) })
            const tags = result.place ? F.placeTags(result.place.tags) : ""
            out.push({ who: "them", text: them + " предложил " + (result.place ? result.place.name : "место"),
                       detail: [result.place && result.place.city, tags, result.place && result.place.rating ? "★ " + result.place.rating : ""]
                               .filter(x => x).join(" · ") })
            out.push({ who: "me", done: true, text: me + " принял место и время", detail: result.message || "" })
        } else {
            out.push({ who: "them", text: them + " проверил календарь " + F.genitive(theirName),
                       detail: "в этом окне общего времени нет" })
            const n = (result.counter_slots || []).length
            out.push({ who: "them", fail: true, text: n ? "Встречные варианты: " + n : "Встречных вариантов нет",
                       detail: result.message || "нужно ваше решение" })
        }
        return out
    }

    // Шаги «проигрываются» только для нового раунда; бронь того же раунда
    // меняет ответ, но анимацию не перезапускает.
    property string shownRound: ""
    onResultChanged: {
        // hasResult здесь может быть ещё не пересчитан — проверяем ответ напрямую.
        if (!result || result.target === undefined || result.at === shownRound) return
        shownRound = result.at
        revealed = 0
        composing = false
        revealTimer.restart()
    }
    Component.onCompleted: {
        shownRound = hasResult ? result.at : ""
        revealed = steps.length
    }

    Timer {
        id: revealTimer
        interval: 900
        repeat: true
        onTriggered: {
            if (page.revealed < page.steps.length) page.revealed++
            else stop()
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: body.implicitHeight + 80
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: body
            x: 44
            y: 40
            width: parent.width - 88
            spacing: 26

            // ------------------------------------------------------------ шапка
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Text {
                    text: "Организация встречи"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: 26
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                StatusPill {
                    visible: page.hasResult || (App && App.busy)
                    label: App && App.busy ? "Идут" : page.booked ? "Забронировано"
                           : page.accepted ? "Ждёт решения" : "Нужен ваш выбор"
                    tone: page.booked ? "green" : page.hasResult && !page.accepted ? "amber" : "accent"
                    icon: page.booked ? "check" : page.hasResult && !page.accepted ? "clock" : "sparkles"
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: page.hasResult
                    text: "Раунд " + page.round + " из " + page.maxRounds
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    font.weight: Font.DemiBold
                    color: SelfvoyTheme.text2
                }
                GlassButton {
                    visible: page.hasResult && !page.composing
                    text: "Новые"
                    symbol: "plus"
                    implicitHeight: 40
                    onClicked: page.composing = true
                }
            }

            // ------------------------------------------------- сцена переговоров
            RowLayout {
                Layout.fillWidth: true
                visible: page.hasResult && !page.composing
                spacing: 24

                AgentCard {
                    Layout.preferredWidth: 250
                    Layout.alignment: Qt.AlignTop
                    name: page.myName
                    selfvoyName: "Selfvoy " + F.genitive(page.myName)
                    talking: revealTimer.running && page.revealed < page.steps.length
                             && page.steps[page.revealed].who === "me"
                    facts: [
                        { icon: "utensils", text: App && App.preferences.diet && App.preferences.diet.length
                                                  ? App.preferences.diet.join(", ") : "без ограничений в еде" },
                        { icon: "clock", text: "делится только свободными окнами" }
                    ]
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 14

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        visible: page.hasResult && !!page.result.topic
                        text: "Тема: " + (page.result.topic || "")
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text3
                    }

                    Repeater {
                        model: page.steps
                        delegate: RowLayout {
                            id: step
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            visible: index < page.revealed
                            opacity: visible ? 1 : 0
                            layoutDirection: modelData.who === "them" ? Qt.RightToLeft : Qt.LeftToRight
                            spacing: 12
                            Behavior on opacity { NumberAnimation { duration: SelfvoyTheme.slowMs } }

                            Rectangle {
                                implicitWidth: 34; implicitHeight: 34; radius: 17
                                color: step.modelData.fail ? Qt.alpha(SelfvoyTheme.danger, 0.15) : SelfvoyTheme.accentA(0.15)
                                border.width: 1.5
                                border.color: step.modelData.fail ? SelfvoyTheme.danger : SelfvoyTheme.accentSoft
                                Text {
                                    anchors.centerIn: parent
                                    visible: !step.modelData.done
                                    text: step.index + 1
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontBody
                                    font.weight: Font.Bold
                                    color: SelfvoyTheme.text
                                }
                                Icon { anchors.centerIn: parent; visible: !!step.modelData.done; name: "check"; size: 16; color: SelfvoyTheme.text }
                            }
                            Rectangle {
                                Layout.maximumWidth: 380
                                implicitWidth: Math.min(380, stepCol.implicitWidth + 36)
                                implicitHeight: stepCol.implicitHeight + 18
                                radius: 22
                                color: SelfvoyTheme.surface2
                                border.width: SelfvoyTheme.dark ? 1 : 0
                                border.color: SelfvoyTheme.border
                                ColumnLayout {
                                    id: stepCol
                                    anchors.centerIn: parent
                                    width: parent.width - 36
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: step.modelData.text
                                        wrapMode: Text.WordWrap
                                        horizontalAlignment: step.modelData.who === "them" ? Text.AlignRight : Text.AlignLeft
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontSmall + 1
                                        font.weight: Font.DemiBold
                                        color: SelfvoyTheme.text
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: text.length > 0
                                        text: step.modelData.detail
                                        wrapMode: Text.WordWrap
                                        horizontalAlignment: step.modelData.who === "them" ? Text.AlignRight : Text.AlignLeft
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontMicro + 1
                                        color: SelfvoyTheme.text3
                                    }
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }

                AgentCard {
                    Layout.preferredWidth: 250
                    Layout.alignment: Qt.AlignTop
                    them: true
                    name: page.theirName
                    selfvoyName: "Selfvoy " + F.genitive(page.theirName)
                    talking: revealTimer.running && page.revealed < page.steps.length
                             && page.steps[page.revealed].who === "them"
                    facts: [{ icon: "clock", text: "делится только свободными окнами" }]
                }
            }

            // ------------------------------------------------------------ итог
            Rectangle {
                Layout.fillWidth: true
                visible: page.accepted && !page.composing && page.revealed >= page.steps.length
                implicitHeight: resultRow.implicitHeight + 56
                radius: 26
                color: SelfvoyTheme.accentTint
                border.width: 1
                border.color: page.booked ? Qt.alpha(SelfvoyTheme.success, 0.45) : SelfvoyTheme.accentA(0.45)

                RowLayout {
                    id: resultRow
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 24
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: page.accepted ? F.dayTitle(page.result.slot.start) + ", " + F.time(page.result.slot.start)
                                                  + "\n" + (page.result.place ? page.result.place.name : "") : ""
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: 28
                            font.weight: Font.Bold
                            lineHeight: 1.1
                            color: SelfvoyTheme.text
                            wrapMode: Text.WordWrap
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 18
                            Repeater {
                                model: !page.accepted ? [] : [
                                    { icon: "users", text: "Двое" },
                                    { icon: "clock", text: "до " + F.time(page.result.slot.end) },
                                    { icon: "map-pin", text: page.result.place ? [page.result.place.city, page.result.place.address].filter(x => x).join(", ") : "" },
                                    { icon: "star", text: page.result.place && page.result.place.rating ? "рейтинг " + page.result.place.rating : "" }
                                ].filter(x => x.text)
                                delegate: Row {
                                    required property var modelData
                                    spacing: 6
                                    Icon { name: modelData.icon; size: 15; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        text: modelData.text
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontSmall
                                        color: SelfvoyTheme.text2
                                    }
                                }
                            }
                        }
                        Text {
                            visible: page.booked
                            text: page.booked ? "Столик забронирован" + (page.result.booking.confirmation ? " · код " + page.result.booking.confirmation : "")
                                                + " · встреча в задачах" : ""
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            font.weight: Font.DemiBold
                            color: SelfvoyTheme.success
                        }
                    }
                    GlassButton {
                        visible: !page.booked
                        variant: "accent"
                        text: "Подтвердить и забронировать"
                        implicitHeight: 52
                        busy: App ? App.bookingBusy : false
                        enabled: App && !App.bookingBusy
                        onClicked: App.bookNegotiation()
                    }
                    GlassButton {
                        visible: !page.booked
                        variant: "quiet"
                        text: "Остановить"
                        implicitHeight: 52
                        onClicked: page.composing = true
                    }
                    GlassButton {
                        visible: page.booked
                        text: "Открыть задачи"
                        symbol: "square-check"
                        implicitHeight: 48
                        onClicked: page.navigate("tasks")
                    }
                }
            }

            // ------------------------------------- не договорились: следующий раунд
            GlassPanel {
                Layout.fillWidth: true
                visible: page.hasResult && !page.accepted && !page.composing && page.revealed >= page.steps.length
                radius: 24
                backdrop: null
                base: 0.5
                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 12
                    Text {
                        text: page.round >= page.maxRounds ? "Лимит раундов исчерпан" : "Встречные варианты собеседника"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontLead
                        font.weight: Font.Bold
                        color: SelfvoyTheme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: page.round >= page.maxRounds
                              ? "Оба Selfvoy провели " + page.maxRounds + " раундов без согласия. Дальше решаете вы: напишите собеседнику напрямую."
                              : "Selfvoy не договаривается дальше без вас. Выберите, как продолжить: следующий раунд с другим окном или чат."
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text2
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: page.hasResult ? (page.result.counter_slots || []) : []
                            delegate: Chip {
                                required property var modelData
                                text: F.dayPhrase(modelData.start, false) + ", " + F.time(modelData.start)
                                enabled: false
                            }
                        }
                    }
                    RowLayout {
                        spacing: 10
                        GlassButton {
                            visible: page.round < page.maxRounds
                            variant: "accent"
                            text: "Следующий раунд: две недели"
                            onClicked: App.negotiateAgent(page.result.target, page.result.topic || "", page.result.duration_minutes || 60, F.windowHours("week"))
                        }
                        GlassButton {
                            text: "Написать собеседнику"
                            symbol: "message-circle"
                            onClicked: { App.openChat(page.result.target); page.navigate("contacts") }
                        }
                    }
                }
            }

            // -------------------------------------------------------- новый запрос
            GlassPanel {
                Layout.fillWidth: true
                visible: page.composing
                radius: 26
                backdrop: null
                base: 0.45
                padding: 28

                ColumnLayout {
                    id: form
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 16
                    property string target: page.contacts.length ? page.contacts[0].email : ""
                    property int duration: 60
                    property string window: "weekend"

                    Text {
                        text: "С кем договориться?"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontLead
                        font.weight: Font.Bold
                        color: SelfvoyTheme.text
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Repeater {
                            model: page.contacts
                            delegate: Chip {
                                required property var modelData
                                text: modelData.name
                                active: form.target === modelData.email
                                onClicked: { form.target = modelData.email; other.text = "" }
                            }
                        }
                    }
                    GlassField {
                        id: other
                        Layout.fillWidth: true
                        Layout.maximumWidth: 460
                        placeholderText: "или email собеседника"
                        icon: "mail"
                        onTextChanged: if (text.length) form.target = text
                    }
                    GlassField {
                        id: topic
                        Layout.fillWidth: true
                        Layout.maximumWidth: 460
                        placeholderText: "Тема, например «обсудить стартап»"
                    }
                    RowLayout {
                        spacing: 8
                        Text { text: "Когда:"; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontSmall; color: SelfvoyTheme.text2; Layout.preferredWidth: 90 }
                        Repeater {
                            model: [{ k: "tomorrow", l: "сегодня–завтра" }, { k: "weekend", l: "до конца недели" }, { k: "week", l: "две недели" }]
                            delegate: Chip {
                                required property var modelData
                                text: modelData.l
                                active: form.window === modelData.k
                                onClicked: form.window = modelData.k
                            }
                        }
                    }
                    RowLayout {
                        spacing: 8
                        Text { text: "Длительность:"; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontSmall; color: SelfvoyTheme.text2; Layout.preferredWidth: 90 }
                        Repeater {
                            model: [30, 60, 90]
                            delegate: Chip {
                                required property int modelData
                                text: modelData + " мин"
                                active: form.duration === modelData
                                onClicked: form.duration = modelData
                            }
                        }
                    }
                    RowLayout {
                        Layout.topMargin: 6
                        spacing: 12
                        GlassButton {
                            variant: "accent"
                            text: "Начать переговоры"
                            symbol: "handshake"
                            busy: App ? App.busy : false
                            enabled: form.target.length > 0 && App && !App.busy
                            onClicked: App.negotiateAgent(form.target, topic.text, form.duration, F.windowHours(form.window))
                        }
                        GlassButton {
                            visible: page.hasResult
                            variant: "quiet"
                            text: "Отмена"
                            onClicked: page.composing = false
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: "Оба Selfvoy сверят свободное время и подберут место с учётом питания обоих. Бронь — только после вашего подтверждения."
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontMicro + 1
                        color: SelfvoyTheme.text3
                    }
                }
            }

            // ------------------------------------------------------- история
            ColumnLayout {
                Layout.fillWidth: true
                visible: App && App.negotiationHistory.length > 1
                spacing: 8
                Text {
                    text: "Раунды за сессию"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    font.weight: Font.Bold
                    color: SelfvoyTheme.text
                }
                Repeater {
                    model: App ? App.negotiationHistory.slice(1) : []
                    delegate: Text {
                        required property var modelData
                        text: F.time(modelData.at) + " · " + F.nameFor(modelData.target, App.chats, App.userId) + " · раунд " + modelData.round
                              + " — " + (modelData.accepted ? "согласовано: " + (modelData.place ? modelData.place.name : "") : "без согласия")
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text2
                    }
                }
            }
        }
    }
}
