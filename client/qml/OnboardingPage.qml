import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy

/*
  OnboardingPage — опрос после регистрации (день рождения, аллергии, диета,
  город, транспорт, бюджет). Показывается, пока prefs.onboarded != true
  (флаг ставит сервер при сохранении анкеты). Всё необязательное:
  «Пропустить» скрывает анкету до конца сессии.
*/
Item {
    id: page

    property bool busy: false

    // Карточка по центру окна; если выше окна — прокручивается целиком.
    Flickable {
        anchors.fill: parent
        contentHeight: card.height + 2 * SelfvoyTheme.spaceXl
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Без полосы не видно, что «Сохранить» ниже края окна.
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        GlassPanel {
            id: card
            x: (parent.width - width) / 2
            y: Math.max(SelfvoyTheme.spaceXl, (page.height - height) / 2)
            width: Math.min(page.width - 2 * SelfvoyTheme.spaceLg, 540)
            height: implicitHeight
            radius: 34
            padding: 34
            backdrop: page.Window.window ? page.Window.window.contentItem : null

            ColumnLayout {
                id: column
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: SelfvoyTheme.spaceMd

                SelfvoyEmblem { Layout.alignment: Qt.AlignHCenter; size: 56 }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Расскажите о себе"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: 26
                    font.weight: Font.Bold
                    color: SelfvoyTheme.textPrimary
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Selfvoy использует это для мест, времени и рекомендаций. Всё необязательно и меняется в настройках."
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontSmall
                    color: SelfvoyTheme.textSecondary
                    wrapMode: Text.WordWrap
                }

                GlassField {
                    id: birthdayField
                    Layout.fillWidth: true
                    label: "День рождения (ГГГГ-ММ-ДД)"
                }
                GlassField {
                    id: allergiesField
                    Layout.fillWidth: true
                    label: "Аллергии (через запятую)"
                }

                Text {
                    text: "Диета"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    color: SelfvoyTheme.textSecondary
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: SelfvoyTheme.spaceSm
                    property var selected: []
                    id: dietFlow
                    Repeater {
                        model: [
                            { value: "vegan", label: "Веган" },
                            { value: "vegetarian", label: "Вегетарианец" },
                            { value: "gluten_free", label: "Без глютена" },
                            { value: "lactose_free", label: "Без лактозы" },
                            { value: "halal", label: "Халяль" },
                            { value: "kosher", label: "Кошер" }
                        ]
                        delegate: GlassButton {
                            required property var modelData
                            variant: dietFlow.selected.indexOf(modelData.value) >= 0 ? "accent" : "glass"
                            text: modelData.label
                            onClicked: {
                                const list = dietFlow.selected.slice()
                                const at = list.indexOf(modelData.value)
                                if (at >= 0) list.splice(at, 1)
                                else list.push(modelData.value)
                                dietFlow.selected = list
                            }
                        }
                    }
                }

                Text {
                    text: "Передвижение"
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: SelfvoyTheme.fontBody
                    color: SelfvoyTheme.textSecondary
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: SelfvoyTheme.spaceSm
                    id: transportFlow
                    property string selected: "walk"
                    Repeater {
                        model: [
                            { value: "walk", label: "Пешком" },
                            { value: "bike", label: "Велосипед" },
                            { value: "transit", label: "Транспорт" },
                            { value: "car", label: "Машина" }
                        ]
                        delegate: GlassButton {
                            required property var modelData
                            variant: transportFlow.selected === modelData.value ? "accent" : "glass"
                            text: modelData.label
                            onClicked: transportFlow.selected = modelData.value
                        }
                    }
                }

                GlassField {
                    id: cityField
                    Layout.fillWidth: true
                    label: "Город"
                }
                GlassField {
                    id: budgetField
                    Layout.fillWidth: true
                    label: "Бюджет на встречу, € (например 25)"
                }

                GlassButton {
                    Layout.fillWidth: true
                    variant: "accent"
                    text: "Сохранить"
                    glyph: "✓"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: {
                        page.busy = true
                        const payload = { transport: transportFlow.selected }
                        if (birthdayField.text.trim().length > 0)
                            payload.birthday = birthdayField.text.trim()
                        const allergies = allergiesField.text.split(",")
                                .map(function (item) { return item.trim() })
                                .filter(function (item) { return item.length > 0 })
                        if (allergies.length > 0) payload.allergies = allergies
                        if (dietFlow.selected.length > 0) payload.diet = dietFlow.selected
                        if (cityField.text.trim().length > 0) payload.city = cityField.text.trim()
                        const budget = Number.parseFloat(budgetField.text.replace(",", "."))
                        if (!Number.isNaN(budget)) payload.budget_limit = budget
                        App.savePreferences(payload)
                        App.dismissOnboarding()
                        page.busy = false
                    }
                }
                GlassButton {
                    Layout.fillWidth: true
                    text: "Пропустить"
                    onClicked: App.dismissOnboarding()
                }
            }
        }
    }
}
