import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "../Format.js" as F

/*
  BookingDetailPanel — карточка записи владельца (как BookingSheet в вебе): кто,
  когда, сколько стоит, статус значком и словом, история (запись → оплата →
  напоминания → «Приду» → отмена), действия: написать, отменить встречу или всю
  серию, отметить оплату / «без оплаты» / возврат (booking.ownerCancel,
  booking.paymentSet). Запись берётся из App.guestBookings по id — после ответа
  сервера панель обновляется сама.
*/
SheetPopup {
    id: panel

    property int bookingId: 0
    property string start: ""            // встреча серии, с которой открыли; "" — ближайшая
    property string confirming: ""       // "" | one | all
    property date now: new Date()

    function show(id, occurrenceStart) {
        bookingId = id
        start = occurrenceStart || ""
        confirming = ""
        now = new Date()
        open()
    }

    readonly property var booking: {
        const list = App ? App.guestBookings : []
        for (let i = 0; i < list.length; ++i) if (list[i].id === bookingId) return list[i]
        return null
    }
    readonly property var pages: App ? App.bookingPages : []
    readonly property var page: booking ? F.pageById(pages, booking.page_id) : null
    readonly property var occurrences: booking ? (booking.occurrences || []).slice().sort(
        (a, b) => Date.parse(a.start) - Date.parse(b.start)) : []
    readonly property var upcoming: occurrences.filter(o => o.status === "active" && Date.parse(o.end || o.start) > now.getTime())
    readonly property var focusOcc: {
        if (!booking) return null
        if (start) for (let i = 0; i < occurrences.length; ++i) if (occurrences[i].start === start) return occurrences[i]
        return upcoming.length > 0 ? upcoming[0] : (occurrences.length > 0 ? occurrences[occurrences.length - 1] : null)
    }
    readonly property bool focusUpcoming: !!focusOcc && focusOcc.status === "active" && Date.parse(focusOcc.start) > now.getTime()
    readonly property var payment: booking ? booking.payment : null
    readonly property var payActions: F.paymentActions(payment)
    readonly property int minutes: focusOcc ? Math.round((Date.parse(focusOcc.end) - Date.parse(focusOcc.start)) / 60000) : 0

    title: booking ? (booking.guest_name || "Гость") : "Запись"
    subtitle: booking ? [booking.page_title || "Запись", minutes > 0 ? minutes + " мин" : ""].filter(s => s).join(" · ") : ""
    leading: Component {
        SelfvoyAvatar { name: panel.booking ? panel.booking.guest_name : ""; size: 48 }
    }

    // ------------------------------------------------ когда и статус
    Group {
        Layout.fillWidth: true
        visible: !!panel.focusOcc
        Cell {
            divider: false
            label: panel.focusOcc ? F.dayTitle(panel.focusOcc.start) + ", " + F.dateLabel(F.parse(panel.focusOcc.start)) : ""
            hint: panel.focusOcc ? F.time(panel.focusOcc.start) + "–" + F.time(panel.focusOcc.end)
                                + (panel.occurrences.length > 1 ? " · встреча " + (panel.occurrences.findIndex(o => o.start === panel.focusOcc.start) + 1)
                                                                   + " из " + panel.occurrences.length : "") : ""
            // Главное число — цена услуги из записи; «По абонементу» — без денег.
            value: F.bookingPriceText(panel.booking, panel.page)
        }
        // Оплата онлайн — отдельной строкой: «Депозит 1 000 ₽» и статус значком и словом.
        Cell {
            visible: !!panel.payment
            label: F.paymentText(panel.payment)
            StatusPill {
                readonly property var pay: panel.payment ? F.paymentStatus(panel.payment.status) : null
                status: pay
                visible: !!pay
            }
        }
        Cell {
            label: "Статус"
            StatusPill {
                status: !panel.booking || !panel.focusOcc ? null
                        : panel.booking.status === "active" && panel.focusOcc.status === "active"
                          ? F.occurrenceStatus(panel.booking, panel.focusOcc, panel.now)
                          : F.bookingStatus(panel.booking, panel.now)
            }
        }
        Cell {
            visible: !!panel.booking && panel.booking.package_id > 0
            label: "По абонементу"
            hint: "Занятие списано с пакета клиента"
            Icon { name: "ticket"; size: 18; color: SelfvoyTheme.text2 }
        }
    }

    // ------------------------------------------------ действия
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        GlassButton {
            Layout.fillWidth: true
            variant: "neutral"
            symbol: "mail"
            text: "Написать"
            enabled: !!panel.booking && !!panel.booking.guest_email
            onClicked: Qt.openUrlExternally("mailto:" + encodeURIComponent(panel.booking.guest_email))
        }
        GlassButton {
            Layout.fillWidth: true
            visible: panel.focusUpcoming && !!panel.booking && panel.booking.status === "active"
            variant: "danger"
            symbol: "x"
            text: panel.upcoming.length > 1 ? "Отменить эту" : "Отменить"
            onClicked: panel.confirming = "one"
        }
        GlassButton {
            Layout.fillWidth: true
            visible: panel.upcoming.length > 1 && !!panel.booking && panel.booking.status === "active"
            variant: "danger"
            symbol: "ban"
            text: "Отменить все"
            onClicked: panel.confirming = "all"
        }
    }

    Rectangle {
        Layout.fillWidth: true
        visible: panel.confirming.length > 0 && !!panel.focusOcc
        implicitHeight: confirmRow.implicitHeight + 24
        radius: 14
        color: SelfvoyTheme.dangerSoft
        RowLayout {
            id: confirmRow
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: !panel.focusOcc ? ""
                      : (panel.confirming === "one"
                         ? "Отменить встречу " + F.stamp(panel.focusOcc.start) + "? Гость получит письмо."
                         : "Отменить все предстоящие встречи (" + panel.upcoming.length + ")? Гость получит письмо.")
                        + (panel.payment && panel.payment.status === "paid" && panel.confirming === "all" ? " Оплата вернётся гостю." : "")
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall + 1
                color: SelfvoyTheme.danger
            }
            GlassButton {
                variant: "accent"
                text: "Да, отменить"
                onClicked: {
                    if (panel.confirming === "one") App.cancelBookingOccurrence(panel.bookingId, panel.focusOcc.start)
                    else App.cancelBookingOccurrence(panel.bookingId, "")
                    panel.confirming = ""
                }
            }
            GlassButton { variant: "quiet"; text: "Нет"; onClicked: panel.confirming = "" }
        }
    }

    // ------------------------------------------------ история
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 6
        spacing: 0
        Repeater {
            model: panel.booking ? F.bookingHistory(panel.booking, panel.focusOcc, panel.now) : []
            delegate: RowLayout {
                id: step
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 12
                readonly property string tone: modelData.tone === "done" ? "green" : modelData.tone === "wait" ? "amber"
                                               : modelData.tone === "fail" ? "red" : ""
                Item {
                    Layout.preferredWidth: 22
                    Layout.fillHeight: true
                    implicitHeight: stepText.implicitHeight + 14
                    // Линия истории между точками
                    Rectangle {
                        x: 10; width: 2
                        y: 26; height: Math.max(0, parent.height - 26)
                        color: SelfvoyTheme.border
                    }
                    Rectangle {
                        width: 22; height: 22; radius: 11
                        y: 4
                        color: SelfvoyTheme.toneBg(step.tone)
                        Icon {
                            anchors.centerIn: parent
                            size: 12
                            name: step.modelData.tone === "done" ? "check" : step.modelData.tone === "wait" ? "clock"
                                  : step.modelData.tone === "fail" ? "x" : "bell"
                            color: SelfvoyTheme.toneFg(step.tone)
                        }
                    }
                }
                ColumnLayout {
                    id: stepText
                    Layout.fillWidth: true
                    Layout.topMargin: 5
                    Layout.bottomMargin: 9
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: step.modelData.title
                        wrapMode: Text.WordWrap
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontBody - 1
                        font.weight: Font.DemiBold
                        color: SelfvoyTheme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: step.modelData.detail.length > 0
                        text: step.modelData.detail
                        wrapMode: Text.WordWrap
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text2
                    }
                }
            }
        }
    }

    // ------------------------------------------------ серия
    Group {
        Layout.fillWidth: true
        visible: panel.occurrences.length > 1
        head: "Встречи серии"
        Repeater {
            model: panel.occurrences
            delegate: Cell {
                required property var modelData
                required property int index
                divider: index > 0
                label: F.dayPhrase(modelData.start) + ", " + F.time(modelData.start)
                StatusPill {
                    status: modelData.status === "cancelled" ? { label: "Отменена", tone: "red", icon: "x" }
                            : modelData.confirmed_at ? { label: "Придёт", tone: "green", icon: "check" } : null
                }
            }
        }
    }

    // ------------------------------------------------ контакты
    Group {
        Layout.fillWidth: true
        visible: !!panel.booking && (!!panel.booking.guest_email || !!panel.booking.cc_email || !!panel.booking.guest_comment)
        Cell {
            divider: false
            visible: !!panel.booking && !!panel.booking.guest_email
            label: "Email"
            value: panel.booking ? panel.booking.guest_email || "" : ""
        }
        Cell {
            visible: !!panel.booking && !!panel.booking.cc_email
            label: "Копия напоминаний"
            hint: "Второй получатель, например родитель"
            value: panel.booking ? panel.booking.cc_email || "" : ""
        }
        Cell {
            visible: !!panel.booking && !!panel.booking.guest_comment
            stacked: true
            label: "Комментарий"
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: panel.booking ? "«" + (panel.booking.guest_comment || "") + "»" : ""
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: SelfvoyTheme.text
            }
        }
    }

    // ------------------------------------------------ оплата
    ColumnLayout {
        Layout.fillWidth: true
        visible: panel.payActions.length > 0
        spacing: 8
        Repeater {
            model: panel.payActions
            delegate: GlassButton {
                required property var modelData
                required property int index
                Layout.fillWidth: true
                implicitHeight: 46
                variant: index === 0 && modelData === "paid" ? "accent" : modelData === "refunded" ? "danger" : "glass"
                symbol: modelData === "paid" ? "check" : modelData === "refunded" ? "undo-2" : "minus"
                text: F.PAYMENT_ACTION[modelData]
                onClicked: App.setBookingPayment(panel.bookingId, modelData)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !!panel.payment && panel.payment.method === "yookassa" && panel.payActions.indexOf("refunded") >= 0
            wrapMode: Text.WordWrap
            text: "Отметка «Возврат» не переводит деньги: вернуть оплату через ЮKassa можно, отменив запись целиком."
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            color: SelfvoyTheme.text2
        }
    }
}
