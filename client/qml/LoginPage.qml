import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy

/*
  LoginPage — вход (макет Welcome) и регистрация (макет Create your Selfvoy),
  плюс шаги auth v3: код из письма, двухфакторный вход, восстановление и сброс.

    Вход / Регистрация → (регистрация) Код подтверждения → Вход
    Вход с неподтверждённым email → Код подтверждения → Вход
    Вход с 2FA → Код TOTP или резервный код → Вход
    «Забыли пароль?» → Код сброса → Новый пароль → Вход
*/
Item {
    id: page

    signal loggedIn()

    readonly property bool busy: App ? App.busy : false
    // flow: "login" | "register" | "verify" | "twofactor" | "forgot" | "reset"
    property string flow: "login"
    // Email из регистрации/входа — подставляется в форму входа после подтверждения кода.
    property string lastEmail: ""
    property bool showServer: false
    readonly property bool wideCard: flow === "login"
    // Невысокое окно (ноутбук 1366×768 за вычетом панели задач): плотнее.
    readonly property bool compact: height < 760
    readonly property string timezone: {
        const m = -new Date().getTimezoneOffset()
        const h = Math.floor(Math.abs(m) / 60), mm = Math.abs(m) % 60
        return "UTC " + (m >= 0 ? "+" : "−") + h + ":" + (mm < 10 ? "0" : "") + mm
    }

    // Подпись над полем (макет: «Email», «Password»)
    component FieldLabel: Text {
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontSmall
        font.weight: Font.DemiBold
        color: SelfvoyTheme.textPrimary
    }
    component Note: Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        font.family: SelfvoyTheme.fontFamily
        font.pixelSize: SelfvoyTheme.fontSmall
        color: SelfvoyTheme.textSecondary
    }

    // Карточка выше окна (ноутбук 1366×768, регистрация) — страница прокручивается.
    Flickable {
        id: scroller
        anchors.fill: parent
        contentWidth: width
        contentHeight: stage.height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { policy: scroller.contentHeight > scroller.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

        Item {
            id: stage
            width: scroller.width
            height: Math.max(scroller.height, card.height + 48)

            // ---------------------------------------------- регистрация: орбиты и пятна
            Item {
                anchors.centerIn: card
                width: card.width * 1.55
                height: card.height * 1.25
                visible: page.flow === "register"
                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        if (width <= 0 || height <= 0) return
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.lineWidth = 1.2
                        const ellipse = (rot, rx, ry, color) => {
                            ctx.save()
                            ctx.translate(width / 2, height / 2)
                            ctx.rotate(rot)
                            ctx.scale(1, ry / rx)
                            ctx.beginPath()
                            ctx.arc(0, 0, rx, 0, Math.PI * 2)
                            ctx.restore()
                            ctx.strokeStyle = color
                            ctx.stroke()
                        }
                        ellipse(-0.45, width * 0.48, height * 0.2, "rgba(190,160,255,0.45)")
                        ellipse(0.5, width * 0.46, height * 0.23, "rgba(160,170,255,0.35)")
                    }
                    onWidthChanged: requestPaint()
                }
                Repeater {
                    model: [{ x: 0.12, y: 0.32 }, { x: 0.86, y: 0.78 }]
                    delegate: Canvas {
                        required property var modelData
                        visible: false  // 2026: без свечения за карточкой
                        width: 260; height: 260
                        x: parent.width * modelData.x - width / 2
                        y: parent.height * modelData.y - height / 2
                        onPaint: {
                        if (width <= 0 || height <= 0) return
                            const ctx = getContext("2d")
                            const g = ctx.createRadialGradient(130, 130, 0, 130, 130, 130)
                            g.addColorStop(0, "rgba(124,58,237,0.75)")
                            g.addColorStop(1, "rgba(124,58,237,0)")
                            ctx.fillStyle = g
                            ctx.fillRect(0, 0, width, height)
                        }
                    }
                }
            }

            GlassPanel {
                id: card
                anchors.centerIn: parent
                width: Math.min(page.wideCard ? 720 : 460, parent.width - 32)
                radius: page.wideCard ? 40 : 34
                padding: page.wideCard ? (page.compact ? 26 : page.height < 820 ? 36 : 56) : 34
                backdrop: page.Window.window ? page.Window.window.contentItem : null
                Behavior on width { NumberAnimation { duration: SelfvoyTheme.moveMs; easing.type: SelfvoyTheme.ease } }

                // Внутреннее фиолетовое сияние за формой (макет Welcome)
                Canvas {
                    anchors.fill: parent
                    anchors.margins: -card.padding
                    visible: false  // 2026: текст на сплошном фоне, без сияния
                    onPaint: {
                        if (width <= 0 || height <= 0) return
                        const ctx = getContext("2d")
                        ctx.reset()
                        const g = ctx.createRadialGradient(width * 0.55, height * 0.45, 0, width * 0.55, height * 0.45, width * 0.42)
                        g.addColorStop(0, "rgba(124,58,237,0.42)")
                        g.addColorStop(1, "rgba(124,58,237,0)")
                        ctx.fillStyle = g
                        ctx.fillRect(0, 0, width, height)
                    }
                    onWidthChanged: requestPaint()
                }

                // Точки «окна» (макет регистрации)
                Row {
                    visible: page.flow === "register"
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: -16
                    anchors.rightMargin: -14
                    spacing: 6
                    Repeater {
                        model: [0.55, 0.55, 0.3]
                        delegate: Rectangle { required property real modelData; width: 11; height: 11; radius: 5.5; color: SelfvoyTheme.glass(modelData) }
                    }
                }

                ColumnLayout {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: card.width - card.padding * 2
                    spacing: page.compact ? 9 : 16
                    // Поля широкой карточки входа — узкой колонкой по центру, как в макете.
                    readonly property real formWidth: page.wideCard ? Math.min(340, width) : width

                    // ----------------------------------------------------- шапка
                    SelfvoyEmblem {
                        visible: page.flow !== "register"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 8
                        size: page.compact ? 56 : page.wideCard ? 84 : 64
                    }
                    RowLayout {
                        visible: page.flow === "register"
                        Layout.alignment: Qt.AlignHCenter
                        Layout.bottomMargin: 12
                        spacing: 10
                        Canvas {
                            implicitWidth: 30; implicitHeight: 30
                            onPaint: {
                        if (width <= 0 || height <= 0) return
                                const ctx = getContext("2d")
                                ctx.reset()
                                ctx.fillStyle = "#FFFFFF"
                                ctx.beginPath()
                                ctx.moveTo(15, 2); ctx.lineTo(28, 27); ctx.lineTo(20, 27); ctx.lineTo(15, 17)
                                ctx.lineTo(10, 27); ctx.lineTo(2, 27); ctx.closePath(); ctx.fill()
                            }
                        }
                        Text { text: "Selfvoy"; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontTitle; font.weight: Font.Bold; color: SelfvoyTheme.textPrimary }
                    }

                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: page.flow === "register" ? Text.AlignLeft : Text.AlignHCenter
                        text: ({ login: "Добро пожаловать в Selfvoy", register: "Создайте свой Selfvoy", verify: "Подтверждение email",
                                 twofactor: "Двухфакторный вход", forgot: "Восстановление пароля", reset: "Новый пароль" })[page.flow]
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: page.flow === "register" ? 26 : SelfvoyTheme.fontHero
                        font.weight: Font.Bold
                        color: SelfvoyTheme.textPrimary
                        wrapMode: Text.WordWrap
                    }
                    Note {
                        visible: page.flow !== "register"
                        Layout.bottomMargin: 10
                        text: ({ login: "Ваша персональная сеть ИИ-агентов", verify: "Введите код из письма",
                                 twofactor: "Код из приложения-аутентификатора или резервный код",
                                 forgot: "Отправим код сброса на email", reset: "Придумайте новый пароль" })[page.flow] || ""
                    }

                    // ======================================================= вход
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "login"
                        spacing: 8

                        FieldLabel { text: "Email" }
                        GlassField {
                            id: loginEmail
                            Layout.fillWidth: true
                            placeholderText: "Введите email"
                            icon: "mail"
                            inputMethodHints: Qt.ImhEmailCharactersOnly
                            enabled: !page.busy
                            onAccepted: loginPassword.forceActiveFocus()
                        }
                        FieldLabel { text: "Пароль"; Layout.topMargin: 8 }
                        GlassField {
                            id: loginPassword
                            Layout.fillWidth: true
                            placeholderText: "••••••••"
                            icon: "lock"
                            echoMode: TextInput.Password
                            enabled: !page.busy
                            onAccepted: signIn.clicked()
                        }
                        LinkText {
                            Layout.alignment: Qt.AlignRight
                            Layout.topMargin: 4
                            text: "Забыли пароль?"
                            normalColor: SelfvoyTheme.textPrimary
                            onClicked: { forgotEmail.text = loginEmail.text; page.flow = "forgot" }
                        }
                        GlassButton {
                            id: signIn
                            Layout.fillWidth: true
                            Layout.topMargin: 14
                            variant: "accent"
                            text: "Войти"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: { page.lastEmail = loginEmail.text; App.login(loginEmail.text, loginPassword.text) }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 10
                            spacing: 8

                            GlassButton {
                                Layout.fillWidth: true
                                text: "Google"
                                glyph: "G"
                                enabled: !page.busy
                                onClicked: App.beginOAuth("google")
                            }
                            GlassButton {
                                Layout.fillWidth: true
                                text: "Яндекс"
                                glyph: "Я"
                                enabled: !page.busy
                                onClicked: App.beginOAuth("yandex")
                            }
                            GlassButton {
                                Layout.fillWidth: true
                                text: "GitHub"
                                glyph: "GH"
                                enabled: !page.busy
                                onClicked: App.beginOAuth("github")
                            }
                        }
                        LinkText {
                            Layout.alignment: Qt.AlignRight
                            Layout.topMargin: 4
                            text: "Создать аккаунт"
                            normalColor: SelfvoyTheme.textPrimary
                            onClicked: page.flow = "register"
                        }
                    }

                    // =================================================== регистрация
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "register"
                        spacing: 7

                        FieldLabel { text: "Полное имя" }
                        GlassField { id: regName; Layout.fillWidth: true; placeholderText: "Введите имя и фамилию"; enabled: !page.busy }
                        FieldLabel { text: "Email"; Layout.topMargin: 6 }
                        GlassField { id: regEmail; Layout.fillWidth: true; placeholderText: "Введите email"; inputMethodHints: Qt.ImhEmailCharactersOnly; enabled: !page.busy }
                        FieldLabel { text: "Пароль"; Layout.topMargin: 6 }
                        GlassField {
                            id: regPassword
                            Layout.fillWidth: true
                            placeholderText: "Минимум 8 символов, буквы и цифры"
                            property bool reveal: false
                            action: reveal ? "eye-off" : "eye"
                            echoMode: reveal ? TextInput.Normal : TextInput.Password
                            enabled: !page.busy
                            onActionClicked: reveal = !reveal
                        }
                        FieldLabel { text: "Часовой пояс"; Layout.topMargin: 6 }
                        GlassField {
                            Layout.fillWidth: true
                            text: page.timezone + " · по системе"
                            readOnly: true
                            icon: "clock"
                        }
                        FieldLabel { text: "С чем должен помогать ваш Selfvoy?"; Layout.topMargin: 6 }
                        GlassField { id: regFocus; Layout.fillWidth: true; filled: true; placeholderText: "например, встречи, продуктивность, код"; enabled: !page.busy }
                        FieldLabel { text: "Код приглашения (необязательно)"; Layout.topMargin: 6 }
                        GlassField {
                            id: regRef
                            Layout.fillWidth: true
                            placeholderText: "Например, ABCD2345"
                            maximumLength: 8
                            inputMethodHints: Qt.ImhUppercaseOnly | Qt.ImhNoPredictiveText
                            // Код — 8 символов A–Z/0–9: неверный не отправляется (регистрация пройдёт без приглашения).
                            readonly property bool bad: text.length > 0 && (App ? App.normalizeReferralCode(text) === "" : false)
                            invalid: bad
                            enabled: !page.busy
                            onTextEdited: {
                                const upper = text.toUpperCase()
                                if (upper !== text) text = upper
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: regRef.bad
                            wrapMode: Text.WordWrap
                            text: "Код — 8 латинских букв и цифр; с неверным кодом регистрация пройдёт без приглашения"
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontMicro
                            color: SelfvoyTheme.textMuted
                        }
                        GlassButton {
                            Layout.fillWidth: true
                            Layout.topMargin: 14
                            variant: "accent"
                            text: "Создать аккаунт"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: {
                                page.lastEmail = regEmail.text
                                App.setPendingFocus(regFocus.text)
                                App.registerUser(regName.text, regEmail.text, regPassword.text, regRef.text)
                            }
                        }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 4
                            spacing: 14
                            Text { text: "Пароль — Argon2id, сессия — JWT"; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontMicro; color: SelfvoyTheme.textMuted }
                        }
                        LinkText {
                            Layout.alignment: Qt.AlignRight
                            text: "Уже есть аккаунт?"
                            normalColor: SelfvoyTheme.textPrimary
                            onClicked: page.flow = "login"
                        }
                    }

                    // ============================================= код подтверждения
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "verify"
                        spacing: 12
                        Note { text: "Код отправлен на " + (App ? App.pendingEmail : "") }
                        Note {
                            visible: App && App.devEmailCode !== ""
                            text: "Режим разработки — код: " + (App ? App.devEmailCode : "")
                            color: SelfvoyTheme.warning
                            font.weight: Font.DemiBold
                        }
                        GlassField { id: verifyCode; Layout.fillWidth: true; placeholderText: "6 цифр"; inputMethodHints: Qt.ImhDigitsOnly; enabled: !page.busy; onAccepted: verifyButton.clicked() }
                        GlassButton {
                            id: verifyButton
                            Layout.fillWidth: true
                            variant: "accent"
                            text: "Подтвердить"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: App.verifyEmail(App.pendingEmail, verifyCode.text)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            LinkText { text: "Отправить код ещё раз"; onClicked: App.resendCode(App.pendingEmail) }
                            Item { Layout.fillWidth: true }
                            LinkText { text: "Назад ко входу"; onClicked: page.flow = "login" }
                        }
                    }

                    // ============================================= двухфакторный вход
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "twofactor"
                        spacing: 12
                        GlassField { id: totp; Layout.fillWidth: true; placeholderText: "123456 или XXXXX-XXXXX"; enabled: !page.busy; onAccepted: totpButton.clicked() }
                        CheckBox {
                            id: trustDevice
                            text: "Доверять этому устройству 90 дней"
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            palette.windowText: SelfvoyTheme.textSecondary
                        }
                        GlassButton {
                            id: totpButton
                            Layout.fillWidth: true
                            variant: "accent"
                            text: "Войти"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: App.login2fa(totp.text, trustDevice.checked)
                        }
                        LinkText { Layout.alignment: Qt.AlignHCenter; text: "Отмена"; onClicked: { App.cancelTwoFactor(); page.flow = "login" } }
                    }

                    // ============================================= забыли пароль
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "forgot"
                        spacing: 12
                        GlassField { id: forgotEmail; Layout.fillWidth: true; placeholderText: "Email"; icon: "mail"; enabled: !page.busy; onAccepted: forgotButton.clicked() }
                        GlassButton {
                            id: forgotButton
                            Layout.fillWidth: true
                            variant: "accent"
                            text: "Отправить код"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: App.forgotPassword(forgotEmail.text)
                        }
                        LinkText { Layout.alignment: Qt.AlignHCenter; text: "Назад ко входу"; onClicked: page.flow = "login" }
                    }

                    // ============================================= новый пароль
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.flow === "reset"
                        spacing: 12
                        Note {
                            visible: App && App.devResetCode !== ""
                            text: "Режим разработки — код: " + (App ? App.devResetCode : "")
                            color: SelfvoyTheme.warning
                            font.weight: Font.DemiBold
                        }
                        GlassField { id: resetCode; Layout.fillWidth: true; placeholderText: "Код из письма"; enabled: !page.busy }
                        GlassField {
                            id: newPassword
                            Layout.fillWidth: true
                            placeholderText: "Новый пароль"
                            echoMode: TextInput.Password
                            icon: "lock"
                            enabled: !page.busy
                            onAccepted: resetButton.clicked()
                        }
                        GlassButton {
                            id: resetButton
                            Layout.fillWidth: true
                            variant: "accent"
                            text: "Сменить пароль"
                            busy: page.busy
                            enabled: !page.busy
                            onClicked: App.resetPassword(App.pendingEmail, resetCode.text, newPassword.text)
                        }
                        LinkText { Layout.alignment: Qt.AlignHCenter; text: "Назад ко входу"; onClicked: page.flow = "login" }
                    }

                    // Статус соединения
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        visible: !App || !App.connected
                        text: App ? App.connectionLabel : ""
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontMicro
                        color: SelfvoyTheme.warning
                    }

                    // Адрес сервера: меняется прямо здесь, сохраняется между запусками.
                    LinkText {
                        Layout.alignment: Qt.AlignHCenter
                        text: page.showServer ? "Скрыть адрес сервера" : "Адрес сервера"
                        onClicked: {
                            page.showServer = !page.showServer
                            if (page.showServer) serverField.text = App ? App.serverUrl : ""
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.maximumWidth: parent.formWidth
                        Layout.alignment: Qt.AlignHCenter
                        visible: page.showServer
                        spacing: 8
                        GlassField {
                            id: serverField
                            Layout.fillWidth: true
                            placeholderText: "ws://адрес:9000 или wss://…"
                            onAccepted: serverApply.clicked()
                        }
                        GlassButton {
                            id: serverApply
                            text: "Подключиться"
                            onClicked: if (App && App.changeServerUrl(serverField.text)) page.showServer = false
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: App
        function onAuthenticatedChanged() { if (App.authenticated) page.loggedIn() }
        function onVerificationRequested() { page.flow = "verify" }
        function onResetFlowRequested() { page.flow = "reset" }
        function onTwoFactorRequiredChanged() {
            if (App.twoFactorRequired) page.flow = "twofactor"
            else if (page.flow === "twofactor") page.flow = "login"
        }
        function onStatusMessageChanged() {
            // После удачного подтверждения/сброса — снова вход.
            if (App.statusMessage.indexOf("подтверждён") !== -1
                    || App.statusMessage.indexOf("Пароль изменён — войдите") !== -1) {
                if (page.flow !== "login") {
                    loginEmail.text = App.pendingEmail || page.lastEmail
                    page.flow = "login"
                    loginPassword.forceActiveFocus()
                }
            }
        }
    }
}
