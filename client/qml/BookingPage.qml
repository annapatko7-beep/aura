import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  BookingPage — «Запись»: страницы записи владельца (booking.*) в дизайне 2026.

  Постоянная ссылка #/b/<slug>: люди без Selfvoy сами выбирают свободное время
  (страница гостя — в веб-клиенте). Здесь — страницы со всеми настройками
  (напоминания, подпись Pro, цена/предоплата/политика отмены, пакеты занятий),
  пост со свободными окнами (booking.slotsPost) и записи гостей с карточкой
  записи (оплата, отмена встречи). runAction("new" | "post") — из «Создать».
*/
Item {
    id: page

    signal navigate(string to)

    readonly property var pages: App ? App.bookingPages : []
    readonly property var bookings: App ? App.guestBookings : []
    readonly property bool yookassa: App && App.paymentsStatus && App.paymentsStatus.connected === true
    property string filter: "upcoming"
    property date now: new Date()

    Component.onCompleted: if (App) { App.loadBooking(); App.loadPayments() }

    function runAction(action) {
        if (action === "new") form.openFor(null)
        else if (action === "post") post.openFor(null)
    }

    function link(p) { return p.url && p.url.length > 0 ? p.url : "" }

    function priceLine(p) {
        const parts = []
        if (p.price) parts.push(F.money(p.price, p.currency))
        if (p.prepay === "full") parts.push("предоплата")
        else if (p.prepay === "deposit" && p.deposit) parts.push("депозит " + F.money(p.deposit, p.currency))
        if (p.cancel_hours > 0) parts.push("отмена за " + p.cancel_hours + " ч")
        if ((p.packages || []).length > 0) parts.push("пакетов: " + p.packages.length)
        return parts.join(" · ")
    }

    readonly property var shownBookings: bookings.filter(b => {
        const st = F.bookingStatus(b, page.now)
        if (filter === "upcoming") return st.label === "Предстоит"
        if (filter === "cancelled") return b.status === "cancelled"
        return true
    })

    component Title: Text {
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontTitle
        font.weight: Font.Bold
        color: SelfvoyTheme.text
    }
    component Caption: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontSmall
        color: SelfvoyTheme.text2
    }
    component NumberField: GlassField {
        property int minimum: 0
        property int maximum: 999
        implicitWidth: 96
        horizontalAlignment: TextInput.AlignRight
        validator: IntValidator { bottom: 0; top: 99999 }
        readonly property int number: text.length > 0 ? parseInt(text) : -1
        invalid: text.length > 0 && (number < minimum || number > maximum)
    }

    BookingDetailPanel { id: detail }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 88
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            x: Math.max(32, (parent.width - 960) / 2)
            y: 40
            width: Math.min(960, parent.width - 64)
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: "Запись"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontDisplay
                    font.weight: Font.Black
                    font.letterSpacing: -1
                    color: SelfvoyTheme.text
                }
                GlassButton {
                    visible: page.pages.length > 0
                    symbol: "megaphone"
                    text: "Пост со свободными окнами"
                    onClicked: post.openFor(null)
                }
                GlassButton {
                    text: "Новая страница"
                    symbol: "plus"
                    variant: "accent"
                    onClicked: form.openFor(null)
                }
            }
            Caption {
                text: "Постоянная ссылка: клиенты сами выбирают свободное время в вашем расписании — можно сразу серией раз в неделю."
                font.pixelSize: SelfvoyTheme.fontBody
            }

            // ------------------------------------------------ страницы
            Title { Layout.topMargin: 10; text: "Страницы" }
            Rectangle {
                Layout.fillWidth: true
                visible: App && App.bookingLoaded && page.pages.length === 0
                implicitHeight: 70
                radius: SelfvoyTheme.radiusCard
                color: SelfvoyTheme.surface
                Caption {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    horizontalAlignment: Text.AlignHCenter
                    text: "Создайте первую страницу — например, «Консультация, 30 минут» — и отправьте ссылку клиентам."
                }
            }
            Repeater {
                model: page.pages
                delegate: GlassPanel {
                    id: card
                    required property var modelData
                    property bool confirmDelete: false
                    Layout.fillWidth: true
                    padding: 18

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                implicitWidth: 12; implicitHeight: 12; radius: 4
                                color: SelfvoyTheme.capsuleColor(F.pageColor(card.modelData.id, page.pages))
                            }
                            Text {
                                Layout.fillWidth: true
                                text: card.modelData.title
                                elide: Text.ElideRight
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontLead
                                font.weight: Font.DemiBold
                                color: SelfvoyTheme.text
                            }
                            StatusPill {
                                status: card.modelData.status === "active" ? { label: "Открыта", tone: "green", icon: "check" }
                                                                          : { label: "На паузе", tone: "amber", icon: "clock" }
                            }
                        }
                        Caption {
                            text: [card.modelData.duration_minutes + " мин",
                                   "на " + F.daysLabel(card.modelData.days_ahead) + " вперёд",
                                   "до " + card.modelData.max_per_day + " в день",
                                   card.modelData.max_series > 1 ? "серия до " + card.modelData.max_series : "",
                                   card.modelData.weekends ? "и в выходные" : "только будни"].filter(s => s.length > 0).join(" · ")
                        }
                        Caption { visible: text.length > 0; text: page.priceLine(card.modelData) }
                        Caption {
                            text: [card.modelData.guest_reminders !== false ? "напоминания гостю включены" : "без напоминаний гостю",
                                   card.modelData.hide_branding ? "подпись Selfvoy скрыта" : ""].filter(s => s.length > 0).join(" · ")
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 38
                            radius: 10
                            color: SelfvoyTheme.field
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 4
                                Icon { name: "link"; size: 15; color: SelfvoyTheme.text2 }
                                TextEdit {
                                    Layout.fillWidth: true
                                    readOnly: true
                                    selectByMouse: true
                                    clip: true
                                    text: page.link(card.modelData) || "#/b/" + card.modelData.slug
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall
                                    color: SelfvoyTheme.text
                                }
                                GlassButton {
                                    implicitHeight: 30
                                    text: "Копировать"
                                    symbol: "copy"
                                    textSize: SelfvoyTheme.fontSmall
                                    onClicked: App.copyText(page.link(card.modelData) || "#/b/" + card.modelData.slug)
                                }
                            }
                        }
                        Caption {
                            visible: !card.modelData.url
                            text: "Сервер не знает свой публичный адрес (SELFVOY_PUBLIC_URL) — полную ссылку соберите из адреса веб-клиента."
                        }
                        Caption {
                            text: card.modelData.upcoming > 0
                                  ? "Предстоит: " + card.modelData.upcoming + " " + F.plural(card.modelData.upcoming, "встреча", "встречи", "встреч")
                                  : "Пока никто не записался"
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            GlassButton { text: "Настроить"; symbol: "settings"; onClicked: form.openFor(card.modelData) }
                            GlassButton {
                                text: "Пост"
                                symbol: "megaphone"
                                variant: "quiet"
                                enabled: card.modelData.status === "active"
                                onClicked: post.openFor(card.modelData)
                            }
                            GlassButton {
                                text: card.modelData.status === "active" ? "Пауза" : "Открыть запись"
                                variant: "quiet"
                                onClicked: App.setBookingPagePaused(card.modelData.id, card.modelData.status === "active")
                            }
                            GlassButton {
                                visible: !card.confirmDelete
                                text: "Удалить"
                                variant: "quiet"
                                onClicked: card.confirmDelete = true
                            }
                            GlassButton {
                                visible: card.confirmDelete
                                text: "Да, удалить"
                                variant: "danger"
                                onClicked: { card.confirmDelete = false; App.deleteBookingPage(card.modelData.id) }
                            }
                            GlassButton {
                                visible: card.confirmDelete
                                text: "Нет"
                                variant: "quiet"
                                onClicked: card.confirmDelete = false
                            }
                        }
                    }
                }
            }

            // ------------------------------------------------ записи гостей
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 14
                Title { Layout.fillWidth: true; text: "Записи" }
                Segmented {
                    options: [{ value: "upcoming", label: "Предстоящие" }, { value: "all", label: "Все" },
                              { value: "cancelled", label: "Отменённые" }]
                    value: page.filter
                    onActivated: value => page.filter = value
                }
            }
            Group {
                Layout.fillWidth: true
                Cell {
                    divider: false
                    visible: page.shownBookings.length === 0
                    hint: page.bookings.length === 0 ? "Здесь появятся люди, которые записались по вашим ссылкам."
                                                     : "В этом списке пока пусто."
                }
                Repeater {
                    model: page.shownBookings
                    delegate: Item {
                        id: row
                        required property var modelData
                        required property int index
                        readonly property var occ: (modelData.occurrences || []).slice().sort((a, b) => Date.parse(a.start) - Date.parse(b.start))
                        readonly property var nextOcc: {
                            for (let i = 0; i < occ.length; ++i)
                                if (occ[i].status === "active" && Date.parse(occ[i].start) > page.now.getTime()) return occ[i]
                            return occ.length > 0 ? occ[occ.length - 1] : null
                        }
                        Layout.fillWidth: true
                        implicitHeight: 64
                        Rectangle { anchors.fill: parent; color: hit.containsMouse ? SelfvoyTheme.field : "transparent" }
                        Rectangle {
                            visible: row.index > 0
                            x: 64; width: parent.width - 64; height: 1
                            color: SelfvoyTheme.border
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 12
                            SelfvoyAvatar { name: row.modelData.guest_name || ""; size: 36 }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: (row.modelData.guest_name || "Гость") + " · " + (row.modelData.page_title || "")
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontBody
                                    font.weight: Font.DemiBold
                                    color: SelfvoyTheme.text
                                }
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: (row.nextOcc ? F.dayPhrase(row.nextOcc.start) + ", " + F.time(row.nextOcc.start) : "")
                                          + (row.occ.length > 1 ? " · серия из " + row.occ.length : "")
                                          + (row.modelData.package_id > 0 ? " · по абонементу" : "")
                                          + (row.modelData.reschedules > 0 ? " · переносов: " + row.modelData.reschedules : "")
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall
                                    color: SelfvoyTheme.text2
                                }
                            }
                            StatusPill {
                                visible: !!row.modelData.payment && row.modelData.status === "active"
                                status: row.modelData.payment ? F.paymentStatus(row.modelData.payment.status) : null
                            }
                            StatusPill { status: F.bookingStatus(row.modelData, page.now) }
                            Icon { name: "chevron-right"; size: 16; color: SelfvoyTheme.text3 }
                        }
                        MouseArea {
                            id: hit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: detail.show(row.modelData.id, row.nextOcc ? row.nextOcc.start : "")
                        }
                    }
                }
            }
        }
    }

    // =================================================================== форма страницы
    SheetPopup {
        id: form
        property var editing: null
        property string error: ""
        property bool proError: false
        property int duration: 30
        property int days: 14
        property string currency: "RUB"
        property string prepay: "none"
        property string lateCancel: "allow"
        property var offers: []         // [{id?, title, sessions, price (мин. ед.), valid_days}]

        title: editing ? "Страница записи" : "Новая страница записи"
        subtitle: editing ? editing.title : "Ссылка, по которой клиенты записываются сами"
        sheetWidth: 640

        function openFor(p) {
            editing = p
            error = ""
            proError = false
            titleField.text = p ? p.title : ""
            descriptionField.text = p ? p.description : ""
            duration = p ? p.duration_minutes : 30
            days = p ? p.days_ahead : 14
            perDayField.text = String(p ? p.max_per_day : 4)
            noticeField.text = String(p ? p.notice_hours : 2)
            seriesField.text = String(p ? p.max_series : 1)
            weekendsSwitch.checked = p ? p.weekends === true : false
            priceField.text = F.moneyInput(p ? p.price : 0)
            currency = p && p.currency ? p.currency : "RUB"
            prepay = p && p.prepay ? p.prepay : "none"
            depositField.text = F.moneyInput(p ? p.deposit : 0)
            paymentUrlField.text = p ? p.payment_url || "" : ""
            paymentNoteField.text = p ? p.payment_note || "" : ""
            payWithinField.text = String(p && p.pay_within_minutes ? p.pay_within_minutes : 60)
            cancelHoursField.text = String(p ? p.cancel_hours || 0 : 0)
            lateCancel = p && p.late_cancel ? p.late_cancel : "allow"
            policyField.text = p ? p.policy_text || "" : ""
            remindersSwitch.checked = p ? p.guest_reminders !== false : true
            brandingSwitch.checked = p ? p.hide_branding === true : false
            offers = (p && p.packages ? p.packages : []).map(o => ({ id: o.id, title: o.title, sessions: o.sessions,
                                                                    price: o.price, valid_days: o.valid_days || 90 }))
            open()
        }

        function setOffer(index, key, value) {
            const list = offers.slice()
            const item = Object.assign({}, list[index])
            item[key] = value
            list[index] = item
            offers = list
        }

        function save() {
            const price = F.parseMoney(priceField.text)
            const deposit = F.parseMoney(depositField.text)
            if (price < 0 || deposit < 0) { error = "Сумма — число, например 2000 или 2000,50"; return }
            for (let i = 0; i < offers.length; ++i)
                if (offers[i].price < 0) { error = "Цена пакета — число, например 12000"; return }
            error = ""
            App.saveBookingPage(editing ? editing.id : 0, {
                title: titleField.text,
                description: descriptionField.text,
                duration_minutes: duration,
                days_ahead: days,
                max_per_day: perDayField.number,
                notice_hours: noticeField.number,
                max_series: seriesField.number,
                weekends: weekendsSwitch.checked,
                guest_reminders: remindersSwitch.checked,
                hide_branding: brandingSwitch.checked,
                price: price,
                currency: currency,
                prepay: prepay,
                deposit: prepay === "deposit" ? deposit : 0,
                payment_url: paymentUrlField.text,
                payment_note: paymentNoteField.text,
                pay_within_minutes: payWithinField.number,
                cancel_hours: cancelHoursField.number,
                late_cancel: lateCancel,
                policy_text: policyField.text,
                packages: offers
            })
        }

        Connections {
            target: App
            function onBookingPageSaved() { form.close() }
            function onBookingPageFailed(message) {
                form.error = message
                form.proError = message === F.PRO_REQUIRED_TEXT || message.indexOf("тарифе Pro") >= 0
            }
        }

        // --- основное
        Group {
            Layout.fillWidth: true
            Cell {
                divider: false
                stacked: true
                interactive: true
                GlassField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "Название, например «Урок математики»"
                    maximumLength: 120
                    onTextEdited: form.error = ""
                }
                GlassField {
                    id: descriptionField
                    Layout.fillWidth: true
                    placeholderText: "Описание для клиентов — необязательно"
                    maximumLength: 1000
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Длительность"
                Segmented {
                    options: [{ value: 15, label: "15 мин" }, { value: 30, label: "30 мин" }, { value: 45, label: "45 мин" },
                              { value: 60, label: "1 час" }, { value: 90, label: "1,5 часа" }]
                    value: form.duration
                    onActivated: value => form.duration = value
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Запись открыта на"
                Segmented {
                    options: [{ value: 7, label: "Неделю" }, { value: 14, label: "2 недели" }, { value: 30, label: "Месяц" },
                              { value: 60, label: "2 месяца" }]
                    value: form.days
                    onActivated: value => form.days = value
                }
            }
            Cell { label: "Встреч в день"; hint: "не больше, 1–16"; NumberField { id: perDayField; minimum: 1; maximum: 16 } }
                  interactive: true
            Cell { label: "Запас до встречи, ч"; hint: "записаться можно не позже, 0–168"; NumberField { id: noticeField; minimum: 0; maximum: 168 } }
            Cell { label: "Серия"; hint: "больше 1 — встречи раз в неделю, до 12"; NumberField { id: seriesField; minimum: 1; maximum: 12 } }
                  interactive: true
            Cell {
                label: "Выходные"
                hint: weekendsSwitch.checked ? "Будни и выходные" : "Только будни"
                Toggle { id: weekendsSwitch }
            }
        }

        // --- цена и оплата (F4)
        Group {
            Layout.fillWidth: true
            head: "Цена и оплата"
            foot: form.prepay === "none" ? "Цена видна клиенту на странице записи. Без предоплаты — бесплатно."
                  : "Время держится " + (payWithinField.number > 0 ? payWithinField.number : 60)
                    + " мин — не оплачено к сроку, запись отменится сама."
            Cell {
                  interactive: true
                divider: false
                label: "Цена"
                hint: "0 — не показывать"
                GlassField {
                    id: priceField
                    implicitWidth: 140
                    horizontalAlignment: TextInput.AlignRight
                    placeholderText: "0"
                    invalid: F.parseMoney(text) < 0 || F.parseMoney(text) > F.PRICE_MAX
                }
                Text {
                    text: F.currencySymbol(form.currency)
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    color: SelfvoyTheme.text2
                }
            }
            Cell {
                  interactive: true
                label: "Валюта"
                GlassComboBox {
                    implicitWidth: 230
                    model: F.CURRENCIES
                    textRole: "label"
                    currentIndex: Math.max(0, F.CURRENCIES.findIndex(c => c.value === form.currency))
                    onActivated: index => form.currency = F.CURRENCIES[index].value
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Предоплата"
                pro: true
                Segmented {
                    options: [{ value: "none", label: "Нет" }, { value: "full", label: "Вся сумма" }, { value: "deposit", label: "Депозит" }]
                    value: form.prepay
                    onActivated: value => form.prepay = value
                }
            }
            Cell {
                  interactive: true
                visible: form.prepay === "deposit"
                label: "Депозит"
                hint: "один раз на запись, не больше цены"
                GlassField {
                    id: depositField
                    implicitWidth: 140
                    horizontalAlignment: TextInput.AlignRight
                    placeholderText: "0"
                    invalid: F.parseMoney(text) < 0
                }
            }
            Cell {
                visible: form.prepay !== "none"
                label: page.yookassa ? "Оплата через ЮKassa" : "Подключить ЮKassa"
                hint: page.yookassa ? "Гость платит картой или СБП, статус обновится сам"
                                    : "или укажите ссылку оплаты и реквизиты ниже"
                clickable: !page.yookassa
                onClicked: { form.close(); page.navigate("settings:integrations") }
                StatusPill {
                    visible: page.yookassa
                    status: ({ label: "Подключена", tone: "green", icon: "check" })
                }
            }
            Cell {
                  interactive: true
                visible: form.prepay !== "none"
                stacked: true
                GlassField {
                    id: paymentUrlField
                    Layout.fillWidth: true
                    placeholderText: "Ссылка оплаты: https://…"
                    maximumLength: 500
                    invalid: text.length > 0 && text.indexOf("https://") !== 0
                }
                GlassField {
                    id: paymentNoteField
                    Layout.fillWidth: true
                    placeholderText: "Реквизиты или СБП: +7 900 000-00-00, Т-Банк"
                    maximumLength: 300
                }
            }
            Cell {
                  interactive: true
                visible: form.prepay !== "none"
                label: "Срок оплаты, мин"
                hint: "15–1440"
                NumberField { id: payWithinField; minimum: 15; maximum: 1440 }
            }
        }

        // --- отмена и перенос (F4)
        Group {
            Layout.fillWidth: true
            head: "Отмена и перенос"
            foot: cancelHoursField.number <= 0 ? "Клиент может перенести или отменить встречу в любое время."
                  : "Позже чем за " + cancelHoursField.number + " ч — "
                    + (form.lateCancel === "block" ? "только через вас." : "без возврата оплаты.")
            Cell {
                  interactive: true
                divider: false
                label: "Бесплатная отмена, ч"
                hint: "не позже чем за N часов; 0 — всегда"
                NumberField { id: cancelHoursField; minimum: 0; maximum: 168 }
            }
            Cell {
                  interactive: true
                visible: cancelHoursField.number > 0
                stacked: true
                label: "Позже срока"
                Segmented {
                    options: [{ value: "allow", label: "Отменять можно, без возврата" }, { value: "block", label: "Только через меня" }]
                    value: form.lateCancel
                    onActivated: value => form.lateCancel = value
                }
            }
            Cell {
                  interactive: true
                stacked: true
                GlassField {
                    id: policyField
                    Layout.fillWidth: true
                    placeholderText: "Текст правил для клиента — необязательно"
                    maximumLength: 500
                }
            }
        }

        // --- клиенту (F1, F3)
        Group {
            Layout.fillWidth: true
            head: "Клиенту"
            Cell {
                  interactive: true
                divider: false
                label: "Напоминания"
                hint: "За сутки и за 2 часа — письмом и в Telegram, с кнопкой «Приду»"
                Toggle { id: remindersSwitch }
            }
            Cell {
                  interactive: true
                label: "Скрыть «Запись через Selfvoy»"
                pro: true
                hint: App && F.planView(App.plan) && !F.planView(App.plan).pro && App.plan.enforced
                      ? "Доступно на тарифе Pro" : "Подпись внизу страницы записи и писем"
                Toggle { id: brandingSwitch }
            }
        }

        // --- пакеты занятий (F5)
        Group {
            Layout.fillWidth: true
            head: "Пакеты и абонементы"
            headExtra: Component { ProBadge {} }
            foot: "Клиент покупает пакет при записи и потом записывается по ссылке абонемента. До 5 пакетов, от 2 до 100 занятий."
            Cell {
                divider: false
                visible: form.offers.length === 0
                hint: "Пакетов нет — например, «8 занятий» со скидкой."
            }
            Repeater {
                model: form.offers.length
                delegate: Cell {
                    id: offerCell
                    required property int index
                    readonly property var offer: form.offers[index] || ({})
                    divider: index > 0
                    stacked: true
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        GlassField {
                            Layout.fillWidth: true
                            label: "Название"
                            Component.onCompleted: text = offerCell.offer.title || ""
                            maximumLength: 80
                            onTextEdited: form.setOffer(offerCell.index, "title", text)
                        }
                        GlassField {
                            implicitWidth: 92
                            label: "Занятий"
                            Component.onCompleted: text = offerCell.offer.sessions ? String(offerCell.offer.sessions) : ""
                            validator: IntValidator { bottom: 0; top: 100 }
                            onTextEdited: form.setOffer(offerCell.index, "sessions", parseInt(text || "0"))
                        }
                        GlassField {
                            implicitWidth: 120
                            label: "Цена, " + F.currencySymbol(form.currency)
                            invalid: F.parseMoney(text) <= 0
                            Component.onCompleted: text = F.moneyInput(offerCell.offer.price)
                            onTextEdited: form.setOffer(offerCell.index, "price", F.parseMoney(text))
                        }
                        GlassField {
                            implicitWidth: 92
                            label: "Дней"
                            Component.onCompleted: text = String(offerCell.offer.valid_days || 90)
                            validator: IntValidator { bottom: 0; top: 365 }
                            onTextEdited: form.setOffer(offerCell.index, "valid_days", parseInt(text || "0"))
                        }
                        GlassButton {
                            variant: "neutral"
                            symbol: "trash-2"
                            implicitWidth: 40
                            Accessible.name: "Убрать пакет"
                            onClicked: {
                                const list = form.offers.slice()
                                list.splice(offerCell.index, 1)
                                form.offers = list
                            }
                        }
                    }
                }
            }
            Cell {
                visible: form.offers.length < 5
                label: "Добавить пакет"
                icon: "plus"
                clickable: true
                onClicked: form.offers = form.offers.concat([{ title: "", sessions: 8, price: 0, valid_days: 90 }])
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: form.error.length > 0
            implicitHeight: errorRow.implicitHeight + 20
            radius: 14
            color: form.proError ? SelfvoyTheme.accentTint : SelfvoyTheme.dangerSoft
            RowLayout {
                id: errorRow
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: form.error
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    color: form.proError ? SelfvoyTheme.accent : SelfvoyTheme.danger
                }
                GlassButton {
                    visible: form.proError
                    text: "Пригласить коллегу"
                    onClicked: { form.close(); page.navigate("settings:referral") }
                }
            }
        }
        GlassButton {
            Layout.fillWidth: true
            implicitHeight: 50
            readonly property bool saving: App ? App.bookingSaving : false
            variant: "accent"
            busy: saving
            enabled: !saving
            text: saving ? "Сохраняем…" : (form.editing ? "Сохранить" : "Создать страницу")
            onClicked: form.save()
        }
    }

    // =================================================================== пост со свободными окнами (F6)
    SheetPopup {
        id: post
        property var target: null
        property string format: "telegram"
        property int days: 7
        readonly property var data: App ? App.slotsPost : ({})
        readonly property var activePages: page.pages.filter(p => p.status === "active")

        title: "Пост со свободными окнами"
        subtitle: target ? target.title : ""
        sheetWidth: 600

        function openFor(p) {
            target = p || (activePages.length > 0 ? activePages[0] : null)
            open()
            request()
        }
        function request() { if (target) App.requestSlotsPost(target.id, days, format) }

        Text {
            Layout.fillWidth: true
            visible: !post.target
            wrapMode: Text.WordWrap
            text: "Нет открытых страниц записи — откройте запись или создайте страницу."
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontBody
            color: SelfvoyTheme.text2
        }
        Group {
            Layout.fillWidth: true
            visible: !!post.target
            Cell {
                  interactive: true
                divider: false
                visible: post.activePages.length > 1
                label: "Страница"
                GlassComboBox {
                    implicitWidth: 260
                    model: post.activePages
                    textRole: "title"
                    currentIndex: Math.max(0, post.activePages.findIndex(p => post.target && p.id === post.target.id))
                    onActivated: index => { post.target = post.activePages[index]; post.request() }
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Формат"
                Segmented {
                    options: [{ value: "telegram", label: "Telegram" }, { value: "vk", label: "VK" }, { value: "plain", label: "Простой текст" }]
                    value: post.format
                    onActivated: value => { post.format = value; post.request() }
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Период"
                Segmented {
                    options: [{ value: 3, label: "3 дня" }, { value: 7, label: "Неделя" }, { value: 14, label: "2 недели" }]
                    value: post.days
                    onActivated: value => { post.days = value; post.request() }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: !!post.target
            implicitHeight: Math.max(140, postText.implicitHeight + 28)
            radius: 16
            color: SelfvoyTheme.surface
            border.width: 1
            border.color: SelfvoyTheme.border
            TextEdit {
                id: postText
                x: 14; y: 14
                width: parent.width - 28
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                text: post.data.loading ? "Собираем свободные окна…" : (post.data.error || post.data.text || "")
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: post.data.error ? SelfvoyTheme.danger : post.data.loading ? SelfvoyTheme.text2 : SelfvoyTheme.text
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !!post.target && !post.data.loading && !post.data.error && post.data.text !== undefined
            wrapMode: Text.WordWrap
            text: "Окон в посте: " + (post.data.slots || 0)
                  + (post.data.url ? "" : " · сервер не знает свой публичный адрес — добавьте ссылку записи сами")
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            color: SelfvoyTheme.text2
        }
        GlassButton {
            Layout.fillWidth: true
            implicitHeight: 48
            visible: !!post.target
            variant: "accent"
            symbol: "copy"
            text: "Скопировать текст"
            enabled: !post.data.loading && !post.data.error && !!post.data.text
            onClicked: { App.copyText(post.data.text); post.close() }
        }
    }
}