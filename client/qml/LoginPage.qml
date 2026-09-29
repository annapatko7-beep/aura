import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  LoginPage — вход, регистрация, подтверждение email и восстановление пароля.

  Потоки (auth v3):
    Вход / Регистрация → (регистрация) Код подтверждения → Вход
    Вход с неподтверждённым email → Код подтверждения → Вход
    «Забыли пароль?» → Код сброса → Новый пароль → Вход
*/
Item {
    id: page

    signal loggedIn()

    readonly property bool busy: App ? App.busy : false
    // flow: "auth" (вход/регистрация) | "verify" (код email) | "forgot" | "reset"
    property string flow: "auth"
    readonly property bool registerMode: modeSwitch.currentIndex === 1

    function backToAuth() {
        flow = "auth";
    }

    // Центрируем карточку и мягко двигаем её при переключении режима
    GlassPanel {
        id: card
        anchors.centerIn: parent
        width: Math.min(420, parent.width - AuraTheme.spaceXl * 2)
        backdrop: page.Window.window ? page.Window.window.contentItem : null
        padding: AuraTheme.spaceXl

        ColumnLayout {
            anchors.fill: parent
            spacing: AuraTheme.spaceMd

            // Логотип: светящееся кольцо
            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: AuraTheme.accentA(0.7)

                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.35; duration: 2400; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 1.0; duration: 2400; easing.type: Easing.InOutSine }
                    }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.62
                    height: width
                    radius: width / 2
                    color: "transparent"
                    border.width: 1
                    border.color: AuraTheme.accentA(0.35)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    radius: 5
                    color: AuraTheme.accentSoft
                }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Aura"
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontHero
                font.weight: Font.Light
                font.letterSpacing: 2
                color: AuraTheme.textPrimary
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: page.flow === "auth"
                      ? "Сеть ИИ-агентов: ваша Аура договорится с другими Аурами,\nа вы просто живёте"
                      : (page.flow === "verify" ? "Подтвердите email — введите код из письма"
                        : page.flow === "forgot" ? "Восстановление пароля"
                        : "Придумайте новый пароль")
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontSmall
                color: AuraTheme.textSecondary
                lineHeight: 1.3
            }

            Item { Layout.preferredHeight: AuraTheme.spaceSm; visible: page.flow === "auth" }

            // ============================ Поток «вход / регистрация» ============================
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd
                visible: page.flow === "auth"

                // Переключатель Вход / Регистрация — стеклянная пилюля
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    radius: AuraTheme.radiusPill
                    color: Qt.rgba(1, 1, 1, 0.05)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, AuraTheme.glassBorder)

                    Rectangle {
                        id: pill
                        width: parent.width / 2 - 4
                        height: parent.height - 8
                        y: 4
                        x: modeSwitch.currentIndex === 0 ? 4 : parent.width / 2
                        radius: AuraTheme.radiusPill
                        color: Qt.rgba(1, 1, 1, 0.12)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.18)
                        Behavior on x { NumberAnimation { duration: AuraTheme.moveMs; easing.type: Easing.OutCubic } }
                    }

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: ["Вход", "Регистрация"]
                            delegate: Item {
                                width: (parent ? parent.width : 200) / 2
                                height: parent ? parent.height : 44
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    font.family: AuraTheme.fontFamily
                                    font.pixelSize: AuraTheme.fontSmall
                                    font.weight: modeSwitch.currentIndex === index ? Font.DemiBold : Font.Normal
                                    color: modeSwitch.currentIndex === index
                                           ? AuraTheme.textPrimary : AuraTheme.textSecondary
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: modeSwitch.currentIndex = index
                                }
                            }
                        }
                    }
                }

                // Скрытый хранитель индекса режима
                QtObject {
                    id: modeSwitch
                    property int currentIndex: 0
                }

                GlassField {
                    id: nameField
                    Layout.fillWidth: true
                    label: "Имя"
                    placeholderText: "Как вас зовут"
                    visible: page.registerMode
                    enabled: !page.busy
                }

                GlassField {
                    id: emailField
                    Layout.fillWidth: true
                    label: "Email"
                    placeholderText: "you@example.com"
                    inputMethodHints: Qt.ImhEmailCharactersOnly
                    enabled: !page.busy
                }

                GlassField {
                    id: passwordField
                    Layout.fillWidth: true
                    label: "Пароль"
                    placeholderText: page.registerMode ? "минимум 8 символов, буквы и цифры" : "••••••••"
                    echoMode: TextInput.Password
                    enabled: !page.busy
                    onAccepted: submitButton.clicked()
                }

                Text {
                    Layout.fillWidth: true
                    visible: page.registerMode
                    text: "Пароль хранится как Argon2id (RFC 9106), сессия — JWT + refresh-ротация."
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontMicro
                    color: AuraTheme.textMuted
                    wrapMode: Text.WordWrap
                }

                GlassButton {
                    id: submitButton
                    Layout.fillWidth: true
                    variant: "accent"
                    text: page.registerMode ? "Создать Ауру" : "Войти"
                    glyph: page.registerMode ? "✦" : "→"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: {
                        if (page.registerMode) {
                            App.registerUser(nameField.text, emailField.text, passwordField.text)
                        } else {
                            App.login(emailField.text, passwordField.text)
                        }
                    }
                }

                // Ссылка «Забыли пароль?»
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Забыли пароль?"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.accentSoft
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            forgotEmailField.text = emailField.text;
                            page.flow = "forgot";
                        }
                    }
                }
            }

            // ============================ Поток «код подтверждения» ============================
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd
                visible: page.flow === "verify"

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Код отправлен на " + (App ? App.pendingEmail : "")
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textSecondary
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: App && App.devEmailCode !== ""
                    text: "Режим разработки — код: " + (App ? App.devEmailCode : "")
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    font.weight: Font.DemiBold
                    color: AuraTheme.warning
                }

                GlassField {
                    id: codeField
                    Layout.fillWidth: true
                    label: "Код из письма"
                    placeholderText: "6 цифр"
                    inputMethodHints: Qt.ImhDigitsOnly
                    enabled: !page.busy
                    onAccepted: verifyButton.clicked()
                }

                GlassButton {
                    id: verifyButton
                    Layout.fillWidth: true
                    variant: "accent"
                    text: "Подтвердить"
                    glyph: "✓"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: App.verifyEmail(App.pendingEmail, codeField.text)
                }

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Отправить код ещё раз"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.accentSoft
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: App.resendCode(App.pendingEmail)
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "Назад"
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textSecondary
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.backToAuth()
                        }
                    }
                }
            }

            // ============================ Поток «двухфакторная аутентификация» ============================
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd
                visible: page.flow === "twofactor"

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Двухфакторная аутентификация"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontBody
                    font.weight: Font.DemiBold
                    color: AuraTheme.textPrimary
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Введите 6-значный код из приложения-аутентификатора\nили резервный код вида XXXXX-XXXXX"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textSecondary
                }

                GlassField {
                    id: twoFactorCodeField
                    Layout.fillWidth: true
                    label: "Код подтверждения"
                    placeholderText: "123456 или XXXXX-XXXXX"
                    enabled: !page.busy
                    onAccepted: twoFactorButton.clicked()
                }

                CheckBox {
                    id: trustDeviceCheck
                    Layout.fillWidth: true
                    text: "Запомнить это устройство (90 дней)"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    contentItem: Text {
                        leftPadding: trustDeviceCheck.indicator.width + AuraTheme.spaceSm
                        text: trustDeviceCheck.text
                        font.family: AuraTheme.fontFamily
                        font.pixelSize: AuraTheme.fontSmall
                        color: AuraTheme.textSecondary
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                GlassButton {
                    id: twoFactorButton
                    Layout.fillWidth: true
                    variant: "accent"
                    text: "Подтвердить"
                    glyph: "✓"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: App.login2fa(twoFactorCodeField.text, trustDeviceCheck.checked)
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Назад"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textSecondary
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            App.cancelTwoFactor();
                            page.backToAuth();
                        }
                    }
                }
            }

            // ============================ Поток «забыли пароль» ============================
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd
                visible: page.flow === "forgot"

                GlassField {
                    id: forgotEmailField
                    Layout.fillWidth: true
                    label: "Email аккаунта"
                    placeholderText: "you@example.com"
                    inputMethodHints: Qt.ImhEmailCharactersOnly
                    enabled: !page.busy
                    onAccepted: forgotButton.clicked()
                }

                GlassButton {
                    id: forgotButton
                    Layout.fillWidth: true
                    variant: "accent"
                    text: "Отправить код сброса"
                    glyph: "→"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: App.forgotPassword(forgotEmailField.text)
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Назад ко входу"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textSecondary
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.backToAuth()
                    }
                }
            }

            // ============================ Поток «новый пароль» ============================
            ColumnLayout {
                Layout.fillWidth: true
                spacing: AuraTheme.spaceMd
                visible: page.flow === "reset"

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: App && App.devResetCode !== ""
                    text: "Режим разработки — код: " + (App ? App.devResetCode : "")
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    font.weight: Font.DemiBold
                    color: AuraTheme.warning
                    wrapMode: Text.WordWrap
                }

                GlassField {
                    id: resetCodeField
                    Layout.fillWidth: true
                    label: "Код сброса"
                    placeholderText: "код из письма"
                    enabled: !page.busy
                }

                GlassField {
                    id: newPasswordField
                    Layout.fillWidth: true
                    label: "Новый пароль"
                    placeholderText: "минимум 8 символов, буквы и цифры"
                    echoMode: TextInput.Password
                    enabled: !page.busy
                    onAccepted: resetButton.clicked()
                }

                GlassButton {
                    id: resetButton
                    Layout.fillWidth: true
                    variant: "accent"
                    text: "Установить пароль"
                    glyph: "✓"
                    busy: page.busy
                    enabled: !page.busy
                    onClicked: App.resetPassword(App.pendingEmail, resetCodeField.text, newPasswordField.text)
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: "Назад ко входу"
                    font.family: AuraTheme.fontFamily
                    font.pixelSize: AuraTheme.fontSmall
                    color: AuraTheme.textSecondary
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.backToAuth()
                    }
                }
            }

            // Строка состояния/подсказки
            Text {
                id: statusLine
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: App ? App.statusMessage : ""
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontMicro
                color: AuraTheme.textSecondary
                wrapMode: Text.WordWrap
                visible: text !== ""
            }

            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: App ? App.connectionLabel : ""
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontMicro
                color: App && App.connected ? AuraTheme.textSecondary : AuraTheme.warning
            }
        }
    }

    Connections {
        target: App
        function onAuthenticatedChanged() {
            if (App.authenticated) page.loggedIn()
        }
        function onVerificationRequested() {
            page.flow = "verify";
        }
        function onResetFlowRequested() {
            page.flow = "reset";
        }
        function onTwoFactorRequiredChanged() {
            // Вход потребовал код 2FA — переключаемся на шаг ввода кода.
            if (App.twoFactorRequired) {
                page.flow = "twofactor";
            } else if (page.flow === "twofactor") {
                page.flow = "auth";
            }
        }
        function onStatusMessageChanged() {
            // После удачного подтверждения/сброса возвращаемся ко входу.
            if (App.statusMessage.indexOf("подтверждён") !== -1 ||
                App.statusMessage.indexOf("Пароль изменён — войдите") !== -1) {
                if (page.flow !== "auth") {
                    page.flow = "auth";
                    modeSwitch.currentIndex = 0;
                }
            }
        }
    }
}
