import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  HomePage — «Сегодня», главный экран специалиста (как web/src/pages/Home.tsx).

  Слева: дата, карточка следующей встречи на акценте с кнопками, лента дня —
  цветные капсулы встреч на пунктире, красная линия «сейчас», задачи со сроком
  и пунктирные свободные окна с «Предложить» (текст с окном — в буфер обмена).
  Клик по встрече открывает карточку записи с оплатой (BookingDetailPanel).
  Справа: вопрос ассистенту, ближайшие задачи и переговоры.
  Данные настоящие: booking.list, booking.pageList, tasks.list, booking.get (окна).
*/
Item {
    id: page

    signal navigate(string to)

    property date now: new Date()
    property int dayOffset: 0

    Timer { interval: 30000; running: true; repeat: true; onTriggered: page.now = new Date() }
    Component.onCompleted: if (App) App.loadBooking()

    readonly property var pages: App ? App.bookingPages : []
    readonly property var bookings: App ? App.guestBookings : []
    readonly property date day: F.addDays(F.startOfDay(now), dayOffset)
    readonly property var entries: F.buildDay(bookings, App ? App.tasks : [], App ? App.freeSlots : [], pages, day, now)
    readonly property var next: F.nextUp(bookings, now)
    readonly property var nextPage: next ? F.pageById(pages, next.booking.page_id) : null
    readonly property var openTasks: {
        const list = (App ? App.tasks : []).filter(t => t.status === "pending")
        list.sort((a, b) => (F.taskWhen(a) || "9999").localeCompare(F.taskWhen(b) || "9999"))
        return list.slice(0, 5)
    }
    readonly property string dateText: {
        const d = page.now
        return (F.DAYS[d.getDay()] + ", " + F.dateLabel(d)).toUpperCase()
    }

    function offer(entry) {
        const p = F.pageById(pages, entry.pageId)
        if (!p) return
        const link = p.url || ""
        const text = "Есть свободное окно " + F.dayPhrase(entry.start.toISOString()) + " в " + F.hhmm(entry.start)
                     + " — " + p.title + "." + (link ? " Записаться: " + link : "")
        App.copyText(text)
    }

    function ask(text) {
        const t = text.trim()
        if (!t || !App) return
        App.askAgentGlobal(t)
        command.text = ""
        page.navigate("selfvoy")
    }

    Connections {
        target: App
        function onTranscriptChanged() { if (App.transcript) command.text = App.transcript }
    }

    BookingDetailPanel { id: detail }

    // ------------------------------------------------------------- строка ленты
    component Capsule: Rectangle {
        property color fill: SelfvoyTheme.accentSolid
        property string icon: "book-open"
        property color iconColor: "#FFFFFF"
        implicitWidth: 42; implicitHeight: 42
        radius: 15
        color: fill
        Icon { anchors.centerIn: parent; name: parent.icon; size: 20; color: parent.iconColor }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: grid.implicitHeight + 96
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        GridLayout {
            id: grid
            x: Math.max(32, (parent.width - 1180) / 2)
            y: 40
            width: Math.min(1180, parent.width - 64)
            columns: width > 900 ? 2 : 1
            columnSpacing: 36
            rowSpacing: 28

            // ====================================================== левая колонка
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 640
                Layout.alignment: Qt.AlignTop
                spacing: 0

                Text {
                    text: page.dateText
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.4
                    color: SelfvoyTheme.text2
                }
                Text {
                    Layout.topMargin: 4
                    Layout.bottomMargin: 18
                    text: "Сегодня"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontDisplay
                    font.weight: Font.Black
                    font.letterSpacing: -1
                    color: SelfvoyTheme.text
                }

                // Ждут решения: опасные действия ассистента
                Rectangle {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 14
                    visible: App && App.confirmations.length > 0
                    implicitHeight: 64
                    radius: SelfvoyTheme.radiusCard
                    color: confirmArea.containsMouse ? SelfvoyTheme.surface2 : SelfvoyTheme.surface
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        spacing: 12
                        Icon { name: "triangle-alert"; size: 20; color: SelfvoyTheme.warning }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                text: "Ждут вашего решения: " + (App ? App.confirmations.length : 0)
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontBody
                                font.weight: Font.DemiBold
                                color: SelfvoyTheme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: App && App.confirmations.length > 0
                                      ? F.toolTitle(App.confirmations[0].tool) + " · " + (App.confirmations[0].summary || "") : ""
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                color: SelfvoyTheme.text2
                            }
                        }
                        Icon { name: "chevron-right"; size: 16; color: SelfvoyTheme.text3 }
                    }
                    MouseArea {
                        id: confirmArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.navigate("confirmations")
                    }
                }

                // ------------------------------------------ следующая встреча
                Rectangle {
                    id: nextCard
                    Layout.fillWidth: true
                    implicitHeight: nextColumn.implicitHeight + 32
                    radius: SelfvoyTheme.radiusShell
                    readonly property int colorIndex: page.next ? F.pageColor(page.next.booking.page_id, page.pages) : 1
                    color: page.next ? SelfvoyTheme.capsuleDeepColor(colorIndex) : SelfvoyTheme.surface
                    readonly property color ink: page.next ? "#FFFFFF" : SelfvoyTheme.text

                    ColumnLayout {
                        id: nextColumn
                        x: 18; y: 16
                        width: parent.width - 36
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Text {
                                Layout.fillWidth: true
                                text: page.next ? "Следующая · " + F.untilText(page.next.start, page.now, page.next.end)
                                                : "Следующая встреча"
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                font.weight: Font.DemiBold
                                color: page.next ? Qt.rgba(1, 1, 1, 0.88) : SelfvoyTheme.text2
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: !!page.next
                                Layout.maximumWidth: nextCard.width * 0.45
                                text: page.next ? page.next.booking.page_title || "" : ""
                                elide: Text.ElideRight
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                font.weight: Font.DemiBold
                                color: Qt.rgba(1, 1, 1, 0.88)
                            }
                        }
                        Text {
                            visible: !!page.next
                            Layout.topMargin: 6
                            text: page.next ? F.hhmm(page.next.start) : ""
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: 46
                            font.weight: Font.ExtraBold
                            font.letterSpacing: -2
                            color: "#FFFFFF"
                        }
                        Text {
                            visible: !!page.next
                            Layout.topMargin: 4
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            textFormat: Text.StyledText
                            // Главное число — цена услуги из записи (не сумма оплаты онлайн).
                            readonly property string price: page.next ? F.bookingPriceText(page.next.booking, page.nextPage) : ""
                            readonly property int minutes: page.next ? Math.round((page.next.end - page.next.start) / 60000) : 0
                            text: page.next ? "<b>" + F.escape(page.next.booking.guest_name || "Гость") + "</b> · "
                                              + minutes + " мин" + (price ? " · " + price : "") : ""
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: 16
                            color: "#FFFFFF"
                        }
                        // Оплата онлайн — второстепенно: значок статуса + «Депозит 1 000 ₽ · ждём оплату».
                        RowLayout {
                            readonly property var payment: page.next ? page.next.booking.payment : null
                            readonly property var payStatus: payment ? F.paymentStatus(payment.status) : null
                            visible: !!payment
                            Layout.topMargin: 2
                            Layout.fillWidth: true
                            spacing: 5
                            Icon {
                                visible: !!parent.payStatus
                                name: parent.payStatus ? parent.payStatus.icon : "clock"
                                size: 13
                                color: Qt.rgba(1, 1, 1, 0.88)
                            }
                            Text {
                                Layout.fillWidth: true
                                text: F.paymentLine(parent.payment)
                                elide: Text.ElideRight
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                color: Qt.rgba(1, 1, 1, 0.88)
                            }
                        }
                        // Пустое состояние
                        Text {
                            visible: !page.next
                            Layout.topMargin: 6
                            text: App && !App.bookingLoaded ? "Загружаем записи…" : "Предстоящих встреч нет"
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontTitle
                            font.weight: Font.Bold
                            color: SelfvoyTheme.text
                        }
                        Text {
                            visible: !page.next
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            wrapMode: Text.WordWrap
                            text: page.pages.length === 0 ? "Заведите страницу записи — клиенты будут записываться сами."
                                                          : "Поделитесь ссылкой записи или постом со свободными окнами."
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall + 1
                            color: SelfvoyTheme.text2
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 14
                            spacing: 8
                            GlassButton {
                                Layout.fillWidth: !!page.next
                                variant: page.next ? "onColor" : "accent"
                                text: page.next ? "Подробнее" : (page.pages.length === 0 ? "Создать страницу" : "Пост со свободными окнами")
                                onClicked: {
                                    if (page.next) detail.show(page.next.booking.id, page.next.occurrence.start)
                                    else page.navigate(page.pages.length === 0 ? "booking:new" : "booking:post")
                                }
                            }
                            GlassButton {
                                visible: !!page.next
                                Layout.fillWidth: true
                                variant: "onColor"
                                text: page.next && page.next.booking.guest_email ? "Написать" : "Все записи"
                                onClicked: {
                                    if (page.next.booking.guest_email)
                                        Qt.openUrlExternally("mailto:" + encodeURIComponent(page.next.booking.guest_email))
                                    else page.navigate("booking")
                                }
                            }
                        }
                    }
                }

                // ------------------------------------------ день
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 24
                    Layout.bottomMargin: 10
                    Text {
                        Layout.fillWidth: true
                        text: "День"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontTitle
                        font.weight: Font.Bold
                        color: SelfvoyTheme.text
                    }
                    Text {
                        text: F.daySummary(page.entries, page.pages)
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall + 1
                        color: SelfvoyTheme.text2
                    }
                }
                Segmented {
                    Layout.bottomMargin: 10
                    options: [{ value: 0, label: "Сегодня" }, { value: 1, label: "Завтра" }]
                    value: page.dayOffset
                    onActivated: value => page.dayOffset = value
                }

                Rectangle {
                    Layout.fillWidth: true
                    visible: page.entries.length === 0
                    implicitHeight: 72
                    radius: SelfvoyTheme.radiusCard
                    color: SelfvoyTheme.surface
                    Text {
                        anchors.centerIn: parent
                        text: page.dayOffset === 0 ? "На сегодня ни встреч, ни задач со сроком." : "На завтра пока ничего нет."
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontBody
                        color: SelfvoyTheme.text2
                    }
                }

                // Лента: пунктир позади капсул
                Item {
                    Layout.fillWidth: true
                    visible: page.entries.length > 0
                    implicitHeight: timeline.implicitHeight

                    Column {
                        x: 44 + 10 + 20
                        y: 28
                        height: Math.max(0, parent.height - 56)
                        spacing: 4
                        clip: true
                        Repeater {
                            model: Math.max(0, Math.floor((parent.height) / 6))
                            delegate: Rectangle { width: 2; height: 2; radius: 1; color: SelfvoyTheme.ter }
                        }
                    }

                    ColumnLayout {
                        id: timeline
                        width: parent.width
                        spacing: 0

                        Repeater {
                            model: page.entries
                            delegate: Loader {
                                required property var modelData
                                Layout.fillWidth: true
                                sourceComponent: modelData.kind === "now" ? nowRow
                                               : modelData.kind === "free" ? freeRow
                                               : modelData.kind === "task" ? taskRow : bookingRow
                                property var entry: modelData
                            }
                        }
                    }
                }
            }

            // ====================================================== правая колонка
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 380
                Layout.maximumWidth: grid.columns > 1 ? 440 : 100000
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: grid.columns > 1 ? 80 : 0
                spacing: 22

                // Вопрос ассистенту
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 52
                    radius: 26
                    color: SelfvoyTheme.surface
                    border.width: command.activeFocus ? 1 : 0
                    border.color: SelfvoyTheme.accent
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 6
                        spacing: 8
                        Icon { name: "sparkles"; size: 18; color: SelfvoyTheme.accent }
                        TextField {
                            id: command
                            Layout.fillWidth: true
                            background: null
                            placeholderText: "Спросите ассистента…"
                            placeholderTextColor: SelfvoyTheme.text3
                            color: SelfvoyTheme.text
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontBody
                            selectionColor: SelfvoyTheme.accentA(0.3)
                            onAccepted: page.ask(text)
                        }
                        GlassButton {
                            variant: "accent"
                            implicitWidth: 40; implicitHeight: 40
                            symbol: command.text.trim().length > 0 ? "send-horizontal"
                                    : (App && App.voiceState === "listening" ? "square" : "mic")
                            busy: App ? (App.busy || App.voiceState === "processing") : false
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: command.text.trim().length > 0 ? "Отправить"
                                          : (App && !App.micAvailable ? "Микрофон не найден" : "Сказать голосом")
                            onClicked: {
                                if (command.text.trim().length > 0) page.ask(command.text)
                                else if (App.voiceState === "listening") App.stopListening()
                                else App.startListening()
                            }
                        }
                    }
                }

                // Задачи
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 4
                        Layout.rightMargin: 4
                        Text {
                            Layout.fillWidth: true
                            text: "ЗАДАЧИ"
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            font.letterSpacing: 0.4
                            font.weight: Font.DemiBold
                            color: SelfvoyTheme.text2
                        }
                        LinkText { text: "Все"; onClicked: page.navigate("tasks") }
                    }
                    Group {
                        Layout.fillWidth: true
                        Cell {
                            divider: false
                            visible: page.openTasks.length === 0
                            hint: "Задач нет. Скажите ассистенту, о чём напомнить."
                        }
                        Repeater {
                            model: page.openTasks
                            delegate: Item {
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true
                                implicitHeight: 52
                                Rectangle {
                                    visible: index > 0
                                    x: 52; width: parent.width - 52; height: 1
                                    color: SelfvoyTheme.border
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 16
                                    anchors.rightMargin: 16
                                    spacing: 12
                                    Rectangle {
                                        implicitWidth: 24; implicitHeight: 24; radius: 12
                                        color: checkArea.containsMouse ? SelfvoyTheme.successSoft : "transparent"
                                        border.width: 2
                                        border.color: checkArea.containsMouse ? SelfvoyTheme.success : SelfvoyTheme.ter
                                        Icon {
                                            anchors.centerIn: parent
                                            visible: checkArea.containsMouse
                                            name: "check"; size: 13; color: SelfvoyTheme.success
                                        }
                                        MouseArea {
                                            id: checkArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: App.completeTask(modelData.id)
                                        }
                                        Accessible.name: "Выполнено: " + modelData.title
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.title
                                        elide: Text.ElideRight
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontBody
                                        color: SelfvoyTheme.text
                                    }
                                    Text {
                                        text: {
                                            const when = F.taskWhen(modelData)
                                            if (!when) return ""
                                            return F.sameDay(F.parse(when), page.now) ? F.time(when) : F.dayPhrase(when) + " " + F.time(when)
                                        }
                                        font.family: SelfvoyTheme.fontFamily
                                        font.pixelSize: SelfvoyTheme.fontSmall
                                        color: SelfvoyTheme.text2
                                    }
                                }
                            }
                        }
                    }
                }

                // Переговоры
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 4
                        Layout.rightMargin: 4
                        Text {
                            Layout.fillWidth: true
                            text: "ПЕРЕГОВОРЫ"
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            font.letterSpacing: 0.4
                            font.weight: Font.DemiBold
                            color: SelfvoyTheme.text2
                        }
                        LinkText { text: "Все"; onClicked: page.navigate("negotiations") }
                    }
                    Group {
                        Layout.fillWidth: true
                        Cell {
                            divider: false
                            visible: !App || App.negotiationHistory.length === 0
                            hint: "Попросите ассистента договориться о встрече — переговоры появятся здесь."
                        }
                        Repeater {
                            model: App ? App.negotiationHistory.slice(0, 3) : []
                            delegate: Cell {
                                required property var modelData
                                required property int index
                                divider: index > 0
                                clickable: true
                                label: "Selfvoy " + F.genitive(F.nameFor(modelData.target, App.chats, App.userId))
                                hint: modelData.accepted && modelData.slot
                                      ? F.dayPhrase(modelData.slot.start) + ", " + F.time(modelData.slot.start)
                                        + (modelData.place && modelData.place.name ? " · " + modelData.place.name : "")
                                      : "Общего времени не нашлось"
                                onClicked: page.navigate("negotiations")
                                StatusPill {
                                    status: modelData.booking ? { label: "Забронировано", tone: "green", icon: "check" }
                                            : modelData.accepted ? { label: "Ждёт решения", tone: "amber", icon: "clock" }
                                            : { label: "Без итога", tone: "", icon: "minus" }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------- строки ленты
    Component {
        id: nowRow
        Item {
            implicitHeight: 22
            Text {
                width: 44
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: F.hhmm(page.now)
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontMicro
                font.weight: Font.Bold
                color: SelfvoyTheme.danger
            }
            Rectangle {
                x: 44 + 10 + 16
                anchors.verticalCenter: parent.verticalCenter
                width: 10; height: 10; radius: 5
                color: SelfvoyTheme.danger
            }
            Rectangle {
                x: 44 + 10 + 20
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - x
                height: 2
                color: SelfvoyTheme.danger
            }
        }
    }

    Component {
        id: bookingRow
        Item {
            id: row
            readonly property var e: parent ? parent.entry : null
            implicitHeight: 68
            Rectangle {
                anchors.fill: parent
                radius: 14
                color: rowArea.containsMouse ? SelfvoyTheme.glass(0.04) : "transparent"
            }
            RowLayout {
                anchors.fill: parent
                spacing: 10
                Text {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: row.e ? F.hhmm(row.e.start) : ""
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    font.weight: Font.DemiBold
                    color: SelfvoyTheme.text2
                }
                Capsule {
                    fill: row.e ? SelfvoyTheme.capsuleColor(row.e.color) : SelfvoyTheme.accentSolid
                    icon: row.e && row.e.color === 3 ? "star" : "book-open"
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: row.e ? row.e.booking.guest_name || "Гость" : ""
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: rowArea.containsMouse ? SelfvoyTheme.accent : SelfvoyTheme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: row.e ? (row.e.booking.page_title || "Запись") + " · "
                                      + Math.round((row.e.end - row.e.start) / 60000) + " мин" : ""
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall + 1
                        color: SelfvoyTheme.text2
                    }
                }
                StatusPill { status: row.e ? row.e.status : null }
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: detail.show(row.e.booking.id, row.e.occurrence.start)
            }
        }
    }

    Component {
        id: taskRow
        Item {
            id: row
            readonly property var e: parent ? parent.entry : null
            implicitHeight: 68
            RowLayout {
                anchors.fill: parent
                spacing: 10
                Text {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: row.e ? F.hhmm(row.e.start) : ""
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    font.weight: Font.DemiBold
                    color: SelfvoyTheme.text2
                }
                Capsule {
                    fill: taskHit.containsMouse ? SelfvoyTheme.successSoft : SelfvoyTheme.surface
                    border.width: 2
                    border.color: taskHit.containsMouse ? SelfvoyTheme.success : SelfvoyTheme.ter
                    icon: "check"
                    iconColor: taskHit.containsMouse ? SelfvoyTheme.success : SelfvoyTheme.text2
                    MouseArea {
                        id: taskHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: App.completeTask(row.e.task.id)
                    }
                    Accessible.name: row.e ? "Выполнено: " + row.e.task.title : ""
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: row.e ? row.e.task.title : ""
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        color: SelfvoyTheme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Задача"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall + 1
                        color: SelfvoyTheme.text2
                    }
                }
            }
        }
    }

    Component {
        id: freeRow
        Item {
            id: row
            readonly property var e: parent ? parent.entry : null
            implicitHeight: 68
            RowLayout {
                anchors.fill: parent
                spacing: 10
                Text {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: row.e ? F.hhmm(row.e.start) : ""
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    font.weight: Font.DemiBold
                    color: SelfvoyTheme.text2
                }
                // Пунктирная капсула свободного окна
                Item {
                    implicitWidth: 42; implicitHeight: 42
                    Rectangle { anchors.fill: parent; radius: 15; color: SelfvoyTheme.bg }
                    Canvas {
                        id: dashed
                        anchors.fill: parent
                        property color stroke: SelfvoyTheme.ter
                        onStrokeChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            ctx.strokeStyle = stroke
                            ctx.lineWidth = 2
                            ctx.setLineDash([3, 3])
                            const r = 14, w = width - 2, h = height - 2
                            ctx.beginPath()
                            ctx.moveTo(1 + r, 1)
                            ctx.arcTo(1 + w, 1, 1 + w, 1 + h, r)
                            ctx.arcTo(1 + w, 1 + h, 1, 1 + h, r)
                            ctx.arcTo(1, 1 + h, 1, 1, r)
                            ctx.arcTo(1, 1, 1 + w, 1, r)
                            ctx.closePath()
                            ctx.stroke()
                        }
                    }
                    Icon { anchors.centerIn: parent; name: "plus"; size: 20; color: SelfvoyTheme.text2 }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: "Свободно"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.Medium
                        color: SelfvoyTheme.text2
                    }
                    Text {
                        text: row.e ? F.hhmm(row.e.start) + "–" + F.hhmm(row.e.end) : ""
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall + 1
                        color: SelfvoyTheme.text2
                    }
                }
                GlassButton {
                    implicitHeight: 30
                    textSize: SelfvoyTheme.fontSmall + 1
                    leftPadding: 12; rightPadding: 12
                    text: "Предложить"
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    ToolTip.text: "Скопировать текст с этим окном и ссылкой записи"
                    onClicked: page.offer(row.e)
                }
            }
        }
    }
}
