import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  PackagesPage — «Абонементы» (F5): пакеты занятий клиентов владельца.

  Список (package.list) с фильтром по статусу, остаток занятий и срок, оплата
  значком и словом; действия: ±1 занятие (package.adjust), отметить оплату /
  «без оплаты» / возврат (package.paymentSet), отмена (package.cancel).
  «Выдать абонемент» (package.issue) — продажа вне сайта: из предложения
  страницы или свой пакет; ссылка пакета показывается один раз.
*/
Item {
    id: page

    signal navigate(string to)

    property string filter: ""
    readonly property var list: App ? App.packages : []
    readonly property var pages: App ? App.bookingPages : []

    Component.onCompleted: if (App) { App.loadPackages(""); if (!App.bookingLoaded) App.loadBooking() }

    function runAction(action) { if (action === "new") issue.openForm() }

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
                Text {
                    Layout.fillWidth: true
                    text: "Абонементы"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontDisplay
                    font.weight: Font.Black
                    font.letterSpacing: -1
                    color: SelfvoyTheme.text
                }
                GlassButton {
                    variant: "accent"
                    symbol: "plus"
                    text: "Выдать абонемент"
                    enabled: page.pages.length > 0
                    onClicked: issue.openForm()
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: page.pages.length === 0
                      ? "Сначала заведите страницу записи: абонемент привязан к ней, по его ссылке клиент записывается сам."
                      : "Пакеты занятий клиентов: купленные при записи и выданные вами. Занятие списывается при записи, отмена вовремя его возвращает."
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody
                color: SelfvoyTheme.text2
            }

            Flickable {
                Layout.fillWidth: true
                implicitHeight: filterRow.implicitHeight
                contentWidth: filterRow.implicitWidth
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Segmented {
                    id: filterRow
                    options: [{ value: "", label: "Все" }, { value: "active", label: "Действуют" },
                              { value: "pending", label: "Ждут оплаты" }, { value: "used", label: "Закончились" },
                              { value: "expired", label: "Истекли" }, { value: "cancelled", label: "Отменены" }]
                    value: page.filter
                    onActivated: value => { page.filter = value; App.loadPackages(value) }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: App && App.packagesLoaded && page.list.length === 0
                implicitHeight: 72
                radius: SelfvoyTheme.radiusCard
                color: SelfvoyTheme.surface
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: page.filter ? "В этом списке пока пусто." : "Абонементов пока нет. Добавьте пакеты на страницу записи или выдайте абонемент вручную."
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    color: SelfvoyTheme.text2
                }
            }

            Repeater {
                model: page.list
                delegate: GlassPanel {
                    id: card
                    required property var modelData
                    property bool confirmCancel: false
                    readonly property int total: Math.max(0, modelData.sessions_total || 0)
                    readonly property int leftCount: Math.max(0, Math.min(total, modelData.sessions_left || 0))
                    readonly property var payActions: F.paymentActions(modelData.payment)
                    readonly property bool live: modelData.status !== "cancelled"
                    Layout.fillWidth: true
                    padding: 18

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Rectangle {
                                implicitWidth: 42; implicitHeight: 42; radius: 15
                                color: SelfvoyTheme.capsuleColor(F.pageColor(card.modelData.page_id, page.pages))
                                Icon { anchors.centerIn: parent; name: "ticket"; size: 20; color: "#FFFFFF" }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: (card.modelData.guest_name || "Клиент") + " · " + card.modelData.title
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontLead
                                    font.weight: Font.DemiBold
                                    color: SelfvoyTheme.text
                                }
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: [card.modelData.page_title || "",
                                           card.modelData.price ? F.money(card.modelData.price, card.modelData.currency) : "",
                                           card.modelData.offer_id > 0 ? "куплен на странице" : "выдан вами"].filter(s => s).join(" · ")
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall
                                    color: SelfvoyTheme.text2
                                }
                            }
                            StatusPill {
                                visible: !!card.modelData.payment && card.modelData.payment.status !== "paid" && card.modelData.status !== "pending"
                                status: card.modelData.payment ? F.paymentStatus(card.modelData.payment.status) : null
                            }
                            StatusPill { status: F.packageStatus(card.modelData.status) }
                        }

                        // Остаток занятий
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 8
                                radius: 4
                                color: SelfvoyTheme.field
                                Rectangle {
                                    width: card.total > 0 ? parent.width * card.leftCount / card.total : 0
                                    height: parent.height
                                    radius: 4
                                    color: card.leftCount <= 1 && card.live ? SelfvoyTheme.amber : SelfvoyTheme.accentSolid
                                }
                            }
                            Text {
                                text: "осталось " + card.leftCount + " из " + card.total
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall + 1
                                font.weight: Font.DemiBold
                                color: card.leftCount <= 1 && card.live ? SelfvoyTheme.warning : SelfvoyTheme.text
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: F.validityLabel(card.modelData)
                                  + (card.modelData.guest_email ? " · " + card.modelData.guest_email : "")
                                  + (card.modelData.cc_email ? " · копия: " + card.modelData.cc_email : "")
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            color: SelfvoyTheme.text2
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            visible: card.live
                            GlassButton {
                                variant: "neutral"
                                symbol: "minus"
                                text: "Занятие"
                                enabled: card.leftCount > 0
                                ToolTip.visible: hovered
                                ToolTip.text: "Списать одно занятие"
                                onClicked: App.adjustPackage(card.modelData.id, -1)
                            }
                            GlassButton {
                                variant: "neutral"
                                symbol: "plus"
                                text: "Занятие"
                                ToolTip.visible: hovered
                                ToolTip.text: "Добавить одно занятие"
                                onClicked: App.adjustPackage(card.modelData.id, 1)
                            }
                            Repeater {
                                model: card.payActions
                                delegate: GlassButton {
                                    required property var modelData
                                    variant: modelData === "paid" ? "accent" : modelData === "refunded" ? "danger" : "glass"
                                    symbol: modelData === "paid" ? "check" : modelData === "refunded" ? "undo-2" : "minus"
                                    text: F.PAYMENT_ACTION[modelData]
                                    onClicked: App.setPackagePayment(card.modelData.id, modelData)
                                }
                            }
                            GlassButton {
                                visible: !!card.modelData.guest_email
                                variant: "quiet"
                                symbol: "mail"
                                text: "Написать"
                                onClicked: Qt.openUrlExternally("mailto:" + encodeURIComponent(card.modelData.guest_email))
                            }
                            GlassButton {
                                visible: !card.confirmCancel
                                variant: "quiet"
                                text: "Отменить абонемент"
                                onClicked: card.confirmCancel = true
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            visible: card.confirmCancel
                            implicitHeight: cancelRow.implicitHeight + 20
                            radius: 14
                            color: SelfvoyTheme.dangerSoft
                            RowLayout {
                                id: cancelRow
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: "Отменить абонемент? По его ссылке больше не записаться; сделанные записи остаются — их можно отменить в «Записи»."
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                                    color: SelfvoyTheme.danger
                                }
                                GlassButton {
                                    variant: "accent"
                                    text: "Да, отменить"
                                    onClicked: { card.confirmCancel = false; App.cancelPackage(card.modelData.id) }
                                }
                                GlassButton { variant: "quiet"; text: "Нет"; onClicked: card.confirmCancel = false }
                            }
                        }
                    }
                }
            }
        }
    }

    // =================================================================== выдать абонемент
    SheetPopup {
        id: issue
        property var targetPage: null
        property int offerIndex: -1          // -1 — свой пакет
        property string error: ""
        property var result: null            // {package, token, url} — ссылка показывается один раз
        readonly property var offers: targetPage && targetPage.packages ? targetPage.packages : []

        title: result ? "Абонемент выдан" : "Выдать абонемент"
        subtitle: result ? "Ссылку видно только сейчас — сохраните или отправьте клиенту" : "Продажа вне сайта: пакет действует сразу"
        sheetWidth: 560

        function openForm() {
            result = null
            error = ""
            targetPage = page.pages.length > 0 ? page.pages[0] : null
            offerIndex = offers.length > 0 ? 0 : -1
            titleField.text = ""
            sessionsField.text = "8"
            priceField.text = ""
            daysField.text = "90"
            nameField.text = ""
            emailField.text = ""
            ccField.text = ""
            paidSwitch.checked = false
            open()
        }

        function submit() {
            const fields = {
                page_id: targetPage ? targetPage.id : 0,
                guest_name: nameField.text,
                guest_email: emailField.text,
                cc_email: ccField.text,
                paid: paidSwitch.checked
            }
            if (offerIndex >= 0 && offers[offerIndex]) {
                fields.offer_id = offers[offerIndex].id
            } else {
                const price = F.parseMoney(priceField.text)
                if (price < 0) { error = "Цена — число, например 12000"; return }
                fields.title = titleField.text
                fields.sessions = parseInt(sessionsField.text || "0")
                fields.price = price
                fields.valid_days = parseInt(daysField.text || "0")
            }
            error = ""
            App.issuePackage(fields)
        }

        Connections {
            target: App
            function onPackageIssued(result) { issue.result = result }
            function onPackageFailed(message) { issue.error = message }
        }

        // --- результат: ссылка пакета
        ColumnLayout {
            id: resultBox
            Layout.fillWidth: true
            visible: !!issue.result
            spacing: 10
            readonly property string link: !issue.result ? ""
                                           : issue.result.url ? issue.result.url : "#/p/" + (issue.result.token || "")
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: linkText.implicitHeight + 24
                radius: 14
                color: SelfvoyTheme.surface
                border.width: 1
                border.color: SelfvoyTheme.border
                TextEdit {
                    id: linkText
                    x: 12; y: 12
                    width: parent.width - 24
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: resultBox.link
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                    color: SelfvoyTheme.text
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: (issue.result && !issue.result.url
                       ? "Сервер не знает свой публичный адрес — добавьте адрес веб-клиента перед «#/p/…». " : "")
                      + "Клиенту ушло письмо со ссылкой, если указан email. По ссылке он видит остаток и записывается сам."
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontSmall
                color: SelfvoyTheme.text2
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GlassButton {
                    Layout.fillWidth: true
                    variant: "accent"
                    symbol: "copy"
                    text: "Скопировать ссылку"
                    onClicked: App.copyText(resultBox.link)
                }
                GlassButton { variant: "quiet"; text: "Готово"; onClicked: issue.close() }
            }
        }

        // --- форма
        Group {
            Layout.fillWidth: true
            visible: !issue.result
            Cell {
                  interactive: true
                divider: false
                label: "Страница записи"
                GlassComboBox {
                    implicitWidth: 240
                    model: page.pages
                    textRole: "title"
                    currentIndex: Math.max(0, page.pages.findIndex(p => issue.targetPage && p.id === issue.targetPage.id))
                    onActivated: index => {
                        issue.targetPage = page.pages[index]
                        issue.offerIndex = issue.offers.length > 0 ? 0 : -1
                    }
                }
            }
            Cell {
                  interactive: true
                stacked: true
                label: "Пакет"
                Segmented {
                    options: issue.offers.map((o, i) => ({ value: i, label: o.title }))
                             .concat([{ value: -1, label: "Свой пакет" }])
                    value: issue.offerIndex
                    onActivated: value => issue.offerIndex = value
                }
                Text {
                    visible: issue.offerIndex >= 0 && !!issue.offers[issue.offerIndex]
                    text: issue.offerIndex >= 0 && issue.offers[issue.offerIndex]
                          ? F.offerSummary(issue.offers[issue.offerIndex], issue.targetPage ? issue.targetPage.currency : "RUB") : ""
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall
                    color: SelfvoyTheme.text2
                }
            }
            Cell {
                  interactive: true
                visible: issue.offerIndex < 0
                stacked: true
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    GlassField { id: titleField; Layout.fillWidth: true; label: "Название"; placeholderText: "8 занятий"; maximumLength: 80 }
                    GlassField { id: sessionsField; implicitWidth: 90; label: "Занятий"; validator: IntValidator { bottom: 0; top: 100 } }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    GlassField {
                        id: priceField
                        Layout.fillWidth: true
                        label: "Цена, " + F.currencySymbol(issue.targetPage ? issue.targetPage.currency : "RUB") + " (0 — бесплатно)"
                        invalid: F.parseMoney(text) < 0
                    }
                    GlassField { id: daysField; implicitWidth: 120; label: "Дней действует"; validator: IntValidator { bottom: 0; top: 365 } }
                }
            }
        }
        Group {
            Layout.fillWidth: true
            visible: !issue.result
            head: "Клиент"
            Cell {
                  interactive: true
                divider: false
                stacked: true
                GlassField { id: nameField; Layout.fillWidth: true; placeholderText: "Имя клиента"; maximumLength: 80; onTextEdited: issue.error = "" }
                GlassField { id: emailField; Layout.fillWidth: true; placeholderText: "Email — пришлём ссылку на абонемент (необязательно)"; maximumLength: 254 }
                GlassField { id: ccField; Layout.fillWidth: true; placeholderText: "Копия напоминаний, например родителю (необязательно)"; maximumLength: 254 }
            }
            Cell {
                  interactive: true
                label: "Уже оплачен"
                hint: paidSwitch.checked ? "Оплата отмечена сразу" : "Оплату отметите потом в карточке абонемента"
                Toggle { id: paidSwitch }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: issue.error.length > 0 && !issue.result
            wrapMode: Text.WordWrap
            text: issue.error
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall + 1
            color: SelfvoyTheme.danger
        }
        GlassButton {
            Layout.fillWidth: true
            visible: !issue.result
            implicitHeight: 50
            variant: "accent"
            text: "Выдать абонемент"
            enabled: !!issue.targetPage
            onClicked: issue.submit()
        }
    }
}