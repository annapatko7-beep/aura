import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"
import "theme"

/*
  OnboardingPage — опрос после регистрации (день рождения, аллергии, диета,
  город, транспорт, бюджет). Показывается, пока prefs.onboarded != true
  (флаг ставит сервер при сохранении анкеты). Всё необязательное:
  «Пропустить» скрывает анкету до конца сессии.
*/
Rectangle {
    id: page

    color: AuraTheme.bgDeep
    property bool busy: false

    Flickable {
        anchors.fill: parent
        contentHeight: card.implicitHeight + 2 * AuraTheme.spaceXl
        clip: true

        GlassPanel {
            id: card
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * AuraTheme.spaceLg, 520)
            implicitHeight: column.implicitHeight + 2 * AuraTheme.spaceLg

            ColumnLayout {
                id: column
                anchors.fill: parent
                anchors.margins: AuraTheme.spaceLg
                spacing: AuraTheme.spaceMd

                Text {
                    text: "Расскажите о себе"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontTitle
                    color: AuraTheme.textPrimary
                }
                Text {
                    Layout.fillWidth: true
                    text: "Аура использует это для мест, времени и рекомендаций. Всё необязательно и меняется в настройках."
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontMicro
                    color: AuraTheme.textMuted
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
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontBody
                    color: AuraTheme.textSecondary
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: AuraTheme.spaceSm
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
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontBody
                    color: AuraTheme.textSecondary
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: AuraTheme.spaceSm
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
