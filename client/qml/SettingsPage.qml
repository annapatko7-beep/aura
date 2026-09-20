import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  SettingsPage — профиль, предпочтения Ауры, долговременная память, выход.
*/
Item {
    id: page

    signal back()
    signal loggedOut()

    // true — страница встроена в desktop-оболочку: кнопка «назад» не нужна.
    property bool embedded: false

    property var prefs: App ? App.preferences : ({})

    Flickable {
        anchors.fill: parent
        anchors.margins: AuraTheme.spaceLg
        contentHeight: column.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            width: parent.width
            spacing: AuraTheme.spaceMd

            // ---------------------------------------------------- шапка
            RowLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd

                GlassButton {
                    visible: !page.embedded
                    variant: "quiet"
                    glyph: "←"
                    text: ""
                    implicitWidth: 44
                    implicitHeight: 40
                    onClicked: page.back()
                }

                Text {
                    Layout.fillWidth: true
                    text: "Настройки"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontTitle
                    font.weight: Font.DemiBold
                    color: AuraTheme.textPrimary
                }

                GlassButton {
                    variant: "danger"
                    text: "Выйти"
                    glyph: "⎋"
                    implicitHeight: 40
                    onClicked: {
                        App.logout()
                        page.loggedOut()
                    }
                }
            }

            // -------------------------------------------------- профиль
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                RowLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceMd

                    AuraAvatar { name: App ? App.userName : ""; size: 52; online: App ? App.connected : false }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Text {
                            text: App ? App.userName : ""
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontTitle
                            font.weight: Font.DemiBold
                            color: AuraTheme.textPrimary
                        }
                        Text {
                            text: App ? App.userEmail : ""
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }
                        Text {
                            text: "Сервер: " + (App ? App.serverUrl : "") + " · " + (App && App.connected ? "подключено" : "офлайн")
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: App && App.connected ? AuraTheme.textMuted : AuraTheme.warning
                        }
                    }
                }
            }

            // --------------------------------------------- предпочтения
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceMd

                    Text {
                        text: "Что знает ваша Аура"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontBody
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd

                        GlassField {
                            id: cityField
                            Layout.fillWidth: true
                            label: "Город"
                            placeholderText: "Керкраде"
                            text: page.prefs && page.prefs.city ? page.prefs.city : ""
                        }

                        GlassField {
                            id: budgetField
                            Layout.fillWidth: true
                            label: "Бюджет, €"
                            placeholderText: "3"
                            validator: DoubleValidator { bottom: 0; top: 1000; decimals: 2 }
                            text: page.prefs && page.prefs.budget_limit ? String(page.prefs.budget_limit) : ""
                        }
                    }

                    // Диета — стеклянные чипы
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceSm

                        Text {
                            text: "Диета"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }

                        Flow {
                            id: dietFlow
                            Layout.fillWidth: true
                            spacing: AuraTheme.spaceSm

                            property var selected: page.prefs && page.prefs.diet ? page.prefs.diet : []

                            Repeater {
                                model: ["vegan", "vegetarian", "gluten_free", "halal", "без кофеина"]
                                delegate: Rectangle {
                                    id: chip
                                    width: chipText.width + AuraTheme.spaceLg
                                    height: 34
                                    radius: AuraTheme.radiusPill
                                    readonly property bool on: dietFlow.selected.indexOf(modelData) >= 0
                                    color: on ? AuraTheme.accentA(0.25) : Qt.rgba(1, 1, 1, 0.06)
                                    border.width: 1
                                    border.color: on ? AuraTheme.accentA(0.55) : Qt.rgba(1, 1, 1, AuraTheme.glassBorder)
                                    scale: chipArea.pressed ? AuraTheme.pressScale : 1
                                    Behavior on scale { NumberAnimation { duration: AuraTheme.pressMs } }

                                    Text {
                                        id: chipText
                                        anchors.centerIn: parent
                                        text: modelData
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontSmall
                                        color: chip.on ? AuraTheme.textPrimary : AuraTheme.textSecondary
                                    }

                                    MouseArea {
                                        id: chipArea
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            const list = dietFlow.selected.slice()
                                            const at = list.indexOf(modelData)
                                            if (at >= 0) list.splice(at, 1)
                                            else list.push(modelData)
                                            dietFlow.selected = list
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Удобные часы — чипы времени
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceSm

                        Text {
                            text: "Удобные часы для встреч"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }

                        Flow {
                            id: hoursFlow
                            Layout.fillWidth: true
                            spacing: 6

                            property var selected: page.prefs && page.prefs.preferred_hours
                                                   ? page.prefs.preferred_hours : [10, 11, 18]

                            Repeater {
                                model: [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21]
                                delegate: Rectangle {
                                    id: hourChip
                                    width: 40
                                    height: 32
                                    radius: AuraTheme.radiusField
                                    readonly property bool on: hoursFlow.selected.indexOf(modelData) >= 0
                                    color: on ? AuraTheme.accentA(0.25) : Qt.rgba(1, 1, 1, 0.05)
                                    border.width: 1
                                    border.color: on ? AuraTheme.accentA(0.5) : Qt.rgba(1, 1, 1, AuraTheme.glassBorder)
                                    scale: hourArea.pressed ? AuraTheme.pressScale : 1
                                    Behavior on scale { NumberAnimation { duration: AuraTheme.pressMs } }

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData + ":00"
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontMicro
                                        color: hourChip.on ? AuraTheme.textPrimary : AuraTheme.textMuted
                                    }

                                    MouseArea {
                                        id: hourArea
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            const list = hoursFlow.selected.slice()
                                            const at = list.indexOf(modelData)
                                            if (at >= 0) list.splice(at, 1)
                                            else list.push(modelData)
                                            hoursFlow.selected = list
                                        }
                                    }
                                }
                            }
                        }
                    }

                    GlassButton {
                        variant: "accent"
                        text: "Сохранить"
                        glyph: "✓"
                        busy: App ? App.busy : false
                        onClicked: App.savePreferences({
                            city: cityField.text,
                            budget_limit: budgetField.text.length > 0 ? parseFloat(budgetField.text) : 0,
                            diet: dietFlow.selected,
                            preferred_hours: hoursFlow.selected
                        })
                    }
                }
            }

            // -------------------------------------------------- память
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: "Долговременная память"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontBody
                            font.weight: Font.DemiBold
                            color: AuraTheme.textPrimary
                        }
                        GlassButton {
                            variant: "quiet"
                            text: "Обновить"
                            glyph: "⟳"
                            implicitHeight: 34
                            onClicked: App.refreshMemory()
                        }
                    }

                    Repeater {
                        model: App ? App.memory : []
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            height: memoryText.implicitHeight + AuraTheme.spaceMd
                            radius: AuraTheme.radiusField
                            color: Qt.rgba(1, 1, 1, 0.04)
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.08)

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: AuraTheme.spaceSm
                                spacing: AuraTheme.spaceSm

                                Rectangle {
                                    Layout.preferredWidth: 4
                                    Layout.fillHeight: true
                                    radius: 2
                                    color: (modelData && modelData.kind === "preference")
                                           ? AuraTheme.accentSoft : AuraTheme.accent
                                }

                                Text {
                                    id: memoryText
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: modelData ? String(modelData.text || "") : ""
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontSmall
                                    color: AuraTheme.textSecondary
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (App ? App.memory.length : 0) === 0
                        text: "Память пока пуста — Аура запомнит важное из разговоров."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }
            }

            // ------------------------------------------- Голос и озвучка
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceMd

                    Text {
                        text: "Голос и озвучка"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontBody
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }

                    // --- Язык распознавания речи ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        Text {
                            Layout.fillWidth: true
                            text: "Язык распознавания"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }
                        ComboBox {
                            id: micLangBox
                            Layout.preferredWidth: 140
                            model: ["auto", "ru", "en"]
                            currentIndex: Math.max(0, model.indexOf(App ? App.micLanguage : "auto"))
                            onActivated: App.setMicLanguage(model[currentIndex])
                        }
                    }

                    // --- Озвучка ответов (TTS) ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: "Озвучивать ответы Ауры"
                                font.family: AuraTheme.fontFamily
                                font.pixelSize: AuraTheme.fontSmall
                                color: AuraTheme.textPrimary
                            }
                            Text {
                                text: "Синтез речи средствами платформы"
                                font.family: AuraTheme.fontFamily
                                font.pixelSize: AuraTheme.fontMicro
                                color: AuraTheme.textMuted
                            }
                        }
                        Switch {
                            checked: App ? App.ttsEnabled : false
                            onToggled: App.setTtsEnabled(checked)
                        }
                    }

                    // --- Голос ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        visible: App ? App.ttsEnabled : false
                        Text {
                            Layout.fillWidth: true
                            text: "Голос"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }
                        ComboBox {
                            id: voiceBox
                            Layout.preferredWidth: 220
                            model: App ? App.ttsVoices : []
                            currentIndex: Math.max(0, model.indexOf(App ? App.ttsVoice : ""))
                            onActivated: App.setTtsVoice(model[currentIndex])
                        }
                    }

                    // --- Скорость ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        visible: App ? App.ttsEnabled : false
                        Text {
                            Layout.preferredWidth: 90
                            text: "Скорость"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: -1.0
                            to: 1.0
                            value: App ? App.ttsRate : 0.0
                            onMoved: App.setTtsRate(value)
                        }
                    }

                    // --- Громкость ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        visible: App ? App.ttsEnabled : false
                        Text {
                            Layout.preferredWidth: 90
                            text: "Громкость"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontSmall
                            color: AuraTheme.textSecondary
                        }
                        Slider {
                            Layout.fillWidth: true
                            from: 0.0
                            to: 1.0
                            value: App ? App.ttsVolume : 1.0
                            onMoved: App.setTtsVolume(value)
                        }
                    }

                    // --- Только важное ---
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceMd
                        visible: App ? App.ttsEnabled : false
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: "Только важное"
                                font.family: AuraTheme.fontFamily
                                font.pixelSize: AuraTheme.fontSmall
                                color: AuraTheme.textPrimary
                            }
                            Text {
                                text: "Озвучивать только ответы Ауры, не обычные сообщения"
                                font.family: AuraTheme.fontFamily
                                font.pixelSize: AuraTheme.fontMicro
                                color: AuraTheme.textMuted
                            }
                        }
                        Switch {
                            checked: App ? App.ttsImportantOnly : false
                            onToggled: App.setTtsImportantOnly(checked)
                        }
                    }

                    // --- Проверка голоса ---
                    GlassButton {
                        visible: App ? App.ttsEnabled : false
                        variant: "quiet"
                        text: App && App.speaking ? "Остановить" : "Проверить голос"
                        glyph: App && App.speaking ? "■" : "▶"
                        onClicked: (App && App.speaking) ? App.stopSpeaking()
                                                         : App.speakImportant("Привет! Это ваша Аура.")
                    }
                }
            }

            // ------------------------------------------- Активные сессии
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                Component.onCompleted: App.loadSessions()

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: "Активные сессии"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontBody
                            font.weight: Font.DemiBold
                            color: AuraTheme.textPrimary
                        }
                        GlassButton {
                            variant: "quiet"
                            text: "Обновить"
                            glyph: "⟳"
                            implicitHeight: 34
                            onClicked: App.loadSessions()
                        }
                        GlassButton {
                            variant: "danger"
                            text: "Завершить все"
                            glyph: "⏻"
                            implicitHeight: 34
                            onClicked: App.revokeAllSessions()
                        }
                    }

                    Repeater {
                        model: App ? App.sessions : []
                        delegate: Rectangle {
                            Layout.fillWidth: true
                            height: sessionRow.implicitHeight + AuraTheme.spaceMd
                            radius: AuraTheme.radiusField
                            color: Qt.rgba(1, 1, 1, 0.04)
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.08)

                            RowLayout {
                                id: sessionRow
                                anchors.fill: parent
                                anchors.margins: AuraTheme.spaceSm
                                spacing: AuraTheme.spaceSm

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        text: (modelData.device || "устройство") +
                                              (modelData.current ? "  ·  это устройство" : "")
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontSmall
                                        font.weight: modelData.current ? Font.DemiBold : Font.Normal
                                        color: modelData.current ? AuraTheme.accentSoft : AuraTheme.textPrimary
                                    }
                                    Text {
                                        text: (modelData.remote_addr || "") + "  ·  с " +
                                              String(modelData.created_at || "").replace("T", " ").substring(0, 16)
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontMicro
                                        color: AuraTheme.textMuted
                                    }
                                }

                                GlassButton {
                                    visible: !modelData.current
                                    variant: "quiet"
                                    text: "Завершить"
                                    implicitHeight: 30
                                    onClicked: App.revokeSession(modelData.id)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: (App ? App.sessions.length : 0) === 0
                        text: "Список пуст — обновите."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                    }
                }
            }

            // ---------------------------------------------- Безопасность
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceSm

                    Text {
                        text: "Безопасность"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontBody
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }

                    Text {
                        Layout.fillWidth: true
                        text: "Пароль хранится как Argon2id. После смены прочие устройства будут отключены."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                        wrapMode: Text.WordWrap
                    }

                    GlassField {
                        id: oldPasswordField
                        Layout.fillWidth: true
                        label: "Текущий пароль"
                        echoMode: TextInput.Password
                    }

                    GlassField {
                        id: newPasswordField
                        Layout.fillWidth: true
                        label: "Новый пароль"
                        placeholderText: "минимум 8 символов, буквы и цифры"
                        echoMode: TextInput.Password
                    }

                    GlassButton {
                        variant: "accent"
                        text: "Сменить пароль"
                        glyph: "🔒"
                        busy: App ? App.busy : false
                        onClicked: {
                            App.changePassword(oldPasswordField.text, newPasswordField.text);
                            oldPasswordField.text = "";
                            newPasswordField.text = "";
                        }
                    }
                }
            }

            // ---------------------------------------------- Двухфакторная аутентификация (TOTP)
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null
                Component.onCompleted: App.loadTwoFactorStatus()

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceSm

                    Text {
                        text: "Двухфакторная аутентификация (TOTP)"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontBody
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }

                    Text {
                        Layout.fillWidth: true
                        text: App && App.twoFactorEnabled
                              ? "Включена. При входе с нового устройства потребуется код из приложения-аутентификатора."
                              : "Защитите аккаунт вторым фактором: коды из приложения-аутентификатора (Google Authenticator, 1Password и др.)."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textMuted
                        wrapMode: Text.WordWrap
                    }

                    // --- Состояние: выключена — кнопка включения ---
                    GlassButton {
                        visible: App && !App.twoFactorEnabled && !App.twoFactorPending
                        variant: "accent"
                        text: "Включить 2FA"
                        glyph: "🔑"
                        busy: App ? App.busy : false
                        onClicked: App.setup2fa()
                    }

                    // --- Состояние: настройка (секрет выдан, ждём подтверждения) ---
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceSm
                        visible: App && App.twoFactorPending && App.twoFactorSecret !== ""

                        Text {
                            Layout.fillWidth: true
                            text: "1. Добавьте секрет в приложение-аутентификатор (вручную или по URI)."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        GlassField {
                            Layout.fillWidth: true
                            label: "Секрет (base32)"
                            text: App ? App.twoFactorSecret : ""
                            readOnly: true
                        }

                        GlassField {
                            Layout.fillWidth: true
                            label: "otpauth:// URI"
                            text: App ? App.twoFactorUri : ""
                            readOnly: true
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "QR-код будет добавлен позже — пока введите секрет вручную."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "2. Введите текущий код из приложения, чтобы включить 2FA."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textSecondary
                            wrapMode: Text.WordWrap
                        }

                        GlassField {
                            id: confirm2faField
                            Layout.fillWidth: true
                            label: "Код из приложения"
                            placeholderText: "6 цифр"
                            inputMethodHints: Qt.ImhDigitsOnly
                            enabled: !App.busy
                        }

                        GlassButton {
                            Layout.fillWidth: true
                            variant: "accent"
                            text: "Подтвердить и включить"
                            glyph: "✓"
                            busy: App ? App.busy : false
                            onClicked: {
                                App.confirm2fa(confirm2faField.text);
                                confirm2faField.text = "";
                            }
                        }
                    }

                    // --- Резервные коды (показываются один раз после подтверждения) ---
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceSm
                        visible: App && App.recoveryCodes.length > 0

                        Text {
                            Layout.fillWidth: true
                            text: "Сохраните резервные коды — они показываются только один раз!"
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            font.weight: Font.DemiBold
                            color: AuraTheme.warning
                            wrapMode: Text.WordWrap
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: AuraTheme.spaceMd
                            rowSpacing: 2
                            Repeater {
                                model: App ? App.recoveryCodes : []
                                delegate: Text {
                                    text: modelData
                                    font.family: "monospace"
                                    font.pixelSize: AuraTheme.fontSmall
                                    color: AuraTheme.textPrimary
                                    textFormat: Text.PlainText
                                }
                            }
                        }
                    }

                    // --- Состояние: включена — отключение и доверенные устройства ---
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: AuraTheme.spaceSm
                        visible: App && App.twoFactorEnabled

                        Text {
                            Layout.fillWidth: true
                            text: "Осталось резервных кодов: " + (App ? App.recoveryCodesLeft : 0)
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: AuraTheme.spaceSm

                            Text {
                                Layout.fillWidth: true
                                text: "Доверенные устройства"
                                font.family: AuraTheme.fontFamily
                                font.pixelSize: AuraTheme.fontSmall
                                font.weight: Font.DemiBold
                                color: AuraTheme.textPrimary
                            }

                            GlassButton {
                                variant: "quiet"
                                text: "Обновить"
                                implicitHeight: 30
                                onClicked: App.loadTrustedDevices()
                            }
                        }

                        Repeater {
                            model: App ? App.trustedDevices : []
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                height: deviceRow.implicitHeight + AuraTheme.spaceMd
                                radius: AuraTheme.radiusField
                                color: Qt.rgba(1, 1, 1, 0.04)
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.08)

                                RowLayout {
                                    id: deviceRow
                                    anchors.fill: parent
                                    anchors.margins: AuraTheme.spaceSm
                                    spacing: AuraTheme.spaceSm

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Text {
                                            text: modelData.device || "устройство"
                                            font.family: AuraTheme.fontFamily
                                            font.pixelSize: AuraTheme.fontSmall
                                            color: AuraTheme.textPrimary
                                        }
                                        Text {
                                            text: (modelData.remote_addr || "") + "  ·  до " +
                                                  String(modelData.expires_at || "").replace("T", " ").substring(0, 10)
                                            font.family: AuraTheme.fontFamily
                                            font.pixelSize: AuraTheme.fontMicro
                                            color: AuraTheme.textMuted
                                        }
                                    }

                                    GlassButton {
                                        variant: "quiet"
                                        text: "Отозвать"
                                        implicitHeight: 30
                                        onClicked: App.revokeTrustedDevice(modelData.id)
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: (App ? App.trustedDevices.length : 0) === 0
                            text: "Нет доверенных устройств."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                        }

                        GlassButton {
                            Layout.fillWidth: true
                            variant: "quiet"
                            text: "Отозвать все доверенные устройства"
                            onClicked: App.revokeAllTrustedDevices()
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "Отключение 2FA требует подтверждения паролем."
                            font.family: AuraTheme.fontFamily
                            font.pixelSize: AuraTheme.fontMicro
                            color: AuraTheme.textMuted
                            wrapMode: Text.WordWrap
                        }

                        GlassField {
                            id: disable2faPassword
                            Layout.fillWidth: true
                            label: "Пароль для отключения"
                            echoMode: TextInput.Password
                        }

                        GlassButton {
                            Layout.fillWidth: true
                            variant: "quiet"
                            text: "Отключить 2FA"
                            glyph: "🔓"
                            busy: App ? App.busy : false
                            onClicked: {
                                App.disable2fa(disable2faPassword.text);
                                disable2faPassword.text = "";
                            }
                        }
                    }
                }
            }

            // --------------------------------------- Разрешения инструментов (этап 8)
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceMd

                    Text {
                        text: "Разрешения инструментов"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontTitle
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Аура лишь формирует намерение — исполняет сервер. "
                              + "Опасные операции (сообщения, письма, бронирование) по умолчанию "
                              + "требуют вашего подтверждения."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: App ? App.toolPermissions : []

                        delegate: RowLayout {
                            id: toolRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: AuraTheme.spaceMd

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    spacing: AuraTheme.spaceXs
                                    Text {
                                        text: toolRow.modelData.tool
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontSmall
                                        font.weight: Font.DemiBold
                                        color: AuraTheme.textPrimary
                                    }
                                    Rectangle {
                                        visible: toolRow.modelData.dangerous === true
                                        radius: AuraTheme.radiusPill
                                        implicitWidth: dangerLabel.implicitWidth + 14
                                        implicitHeight: 18
                                        color: Qt.rgba(0.98, 0.75, 0.14, 0.12)
                                        border.width: 1
                                        border.color: Qt.rgba(0.98, 0.75, 0.14, 0.40)
                                        Text {
                                            id: dangerLabel
                                            anchors.centerIn: parent
                                            text: "опасный"
                                            font.family: AuraTheme.fontFamily
                                            font.pixelSize: AuraTheme.fontMicro
                                            color: AuraTheme.warning
                                        }
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: toolRow.modelData.description || ""
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontMicro
                                    color: AuraTheme.textMuted
                                    wrapMode: Text.WordWrap
                                }
                            }

                            // Переключатель режима: разрешить / спрашивать / запретить.
                            RowLayout {
                                spacing: AuraTheme.spaceXs

                                Repeater {
                                    model: [
                                        { mode: "allow", label: "Разрешить" },
                                        { mode: "ask", label: "Спрашивать" },
                                        { mode: "deny", label: "Запретить" }
                                    ]

                                    delegate: GlassButton {
                                        required property var modelData
                                        text: modelData.label
                                        variant: toolRow.modelData.mode === modelData.mode ? "accent" : "quiet"
                                        implicitHeight: 32
                                        onClicked: App.setToolPermission(toolRow.modelData.tool, modelData.mode)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // --------------------------------------------- Интеграции (этап 9)
            GlassPanel {
                Layout.fillWidth: true
                backdrop: page.Window.window ? page.Window.window.contentItem : null

                ColumnLayout {
                    anchors.fill: parent
                    spacing: AuraTheme.spaceMd

                    Text {
                        text: "Интеграции"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontTitle
                        font.weight: Font.DemiBold
                        color: AuraTheme.textPrimary
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "Подключите Google Календарь и Gmail через официальный OAuth. "
                              + "Токены хранятся на сервере зашифрованными и не передаются "
                              + "клиенту; доступ можно отозвать в любой момент."
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontMicro
                        color: AuraTheme.textSecondary
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: App ? App.integrationProviders : []

                        delegate: RowLayout {
                            id: providerRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: AuraTheme.spaceMd

                            // Активное подключение этого провайдера (или null).
                            property var connection: {
                                const list = App ? App.integrations : []
                                for (let i = 0; i < list.length; ++i) {
                                    if (list[i].provider === providerRow.modelData.provider)
                                        return list[i]
                                }
                                return null
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    spacing: AuraTheme.spaceXs
                                    Text {
                                        text: providerRow.modelData.name
                                        font.family: AuraTheme.fontFamily
                                        font.pixelSize: AuraTheme.fontSmall
                                        font.weight: Font.DemiBold
                                        color: AuraTheme.textPrimary
                                    }
                                    Rectangle {
                                        visible: providerRow.connection !== null
                                        radius: AuraTheme.radiusPill
                                        implicitWidth: stateLabel.implicitWidth + 14
                                        implicitHeight: 18
                                        color: providerRow.connection
                                               && providerRow.connection.status === "active"
                                               ? Qt.rgba(0.29, 0.87, 0.50, 0.12)
                                               : Qt.rgba(0.98, 0.75, 0.14, 0.12)
                                        border.width: 1
                                        border.color: providerRow.connection
                                                      && providerRow.connection.status === "active"
                                                      ? Qt.rgba(0.29, 0.87, 0.50, 0.40)
                                                      : Qt.rgba(0.98, 0.75, 0.14, 0.40)
                                        Text {
                                            id: stateLabel
                                            anchors.centerIn: parent
                                            text: providerRow.connection
                                                  ? (providerRow.connection.status === "active"
                                                     ? "подключено" : providerRow.connection.status)
                                                  : ""
                                            font.family: AuraTheme.fontFamily
                                            font.pixelSize: AuraTheme.fontMicro
                                            color: providerRow.connection
                                                   && providerRow.connection.status === "active"
                                                   ? AuraTheme.success : AuraTheme.warning
                                        }
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: providerRow.connection
                                          && providerRow.connection.account
                                          ? providerRow.modelData.description + " · "
                                            + providerRow.connection.account
                                          : providerRow.modelData.description
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontMicro
                                    color: AuraTheme.textMuted
                                    wrapMode: Text.WordWrap
                                }
                            }

                            RowLayout {
                                spacing: AuraTheme.spaceXs

                                GlassButton {
                                    visible: providerRow.connection === null
                                    text: "Подключить"
                                    variant: "accent"
                                    implicitHeight: 32
                                    onClicked: App.beginIntegration(providerRow.modelData.provider)
                                }
                                GlassButton {
                                    visible: providerRow.connection !== null
                                    text: "Синхронизировать"
                                    variant: "quiet"
                                    implicitHeight: 32
                                    onClicked: App.syncIntegration(providerRow.connection.id)
                                }
                                GlassButton {
                                    visible: providerRow.connection !== null
                                    text: "Отключить"
                                    variant: "quiet"
                                    implicitHeight: 32
                                    onClicked: App.revokeIntegration(providerRow.connection.id)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
