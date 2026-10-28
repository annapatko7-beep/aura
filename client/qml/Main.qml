import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtCore
import Selfvoy

/*
  Главное окно Qt-клиента Selfvoy (v3).

  Фон — SelfvoyBackground (сплошной цвет темы 2026: светлая или тёмная).
  Маршрут: вход → онбординг-анкета → оболочка с сайдбаром (DesktopShell сама
  сворачивает сайдбар в рельс в узком окне).
*/
ApplicationWindow {
    id: window

    width: 1360
    height: 880
    minimumWidth: 760
    minimumHeight: 600
    visible: true
    color: SelfvoyTheme.bgDeep
    title: App && App.authenticated ? "Selfvoy — " + App.userName : "Selfvoy"
    font.family: SelfvoyTheme.fontFamily

    // Окно не должно быть больше экрана (ноутбуки 1366×768, сохранённая
    // геометрия с большого монитора): ужимаем и возвращаем в видимую область.
    function fitToScreen() {
        const aw = Screen.desktopAvailableWidth
        const ah = Screen.desktopAvailableHeight - 40   // рамка и заголовок окна
        if (aw <= 0 || ah <= 0) return
        if (width > aw) width = Math.max(minimumWidth, aw)
        if (height > ah) height = Math.max(minimumHeight, ah)
        if (x < 0 || x + width > aw) x = Math.max(0, Math.round((aw - width) / 2))
        if (y < 0 || y + height > ah + 40) y = Math.max(0, Math.round((ah - height) / 2))
    }
    Component.onCompleted: Qt.callLater(fitToScreen)

    // Desktop-учтивость: запоминаем геометрию окна между запусками.
    Settings {
        property alias winX: window.x
        property alias winY: window.y
        property alias winWidth: window.width
        property alias winHeight: window.height
    }

    SelfvoyBackground {
        id: backdrop
        anchors.fill: parent
    }

    Loader {
        id: rootLoader
        anchors.fill: parent
        sourceComponent: !App || !App.authenticated ? loginPage
                         : App.needOnboarding ? onboardingPage
                         : desktopShell
    }

    Component {
        id: loginPage
        LoginPage {}
    }

    // Онбординг-опрос после регистрации: пока prefs.onboarded не выставлен.
    Component {
        id: onboardingPage
        OnboardingPage {}
    }

    Component {
        id: desktopShell
        DesktopShell {}
    }

    // ------------------------------------------------------------- тосты
    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: SelfvoyTheme.spaceXl
        width: Math.min(parent.width - SelfvoyTheme.spaceXl * 2, toastText.implicitWidth + SelfvoyTheme.spaceLg * 2)
        height: 46
        radius: SelfvoyTheme.radiusPill
        color: toast.isError ? SelfvoyTheme.danger : SelfvoyTheme.text
        opacity: 0
        visible: opacity > 0
        z: 100

        property bool isError: false

        Behavior on opacity { NumberAnimation { duration: SelfvoyTheme.moveMs } }

        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - SelfvoyTheme.spaceLg * 2)
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontSmall
            font.weight: Font.Medium
            color: toast.isError ? "#FFFFFF" : SelfvoyTheme.bg
            text: toast.isError ? (App ? App.errorMessage : "") : (App ? App.statusMessage : "")
        }
    }

    Timer {
        id: toastTimer
        interval: 3600
        onTriggered: toast.opacity = 0
    }

    Connections {
        target: App
        function onErrorMessageChanged() {
            if (App.errorMessage.length > 0) {
                toast.isError = true
                toast.opacity = 1
                toastTimer.restart()
            }
        }
        function onStatusMessageChanged() {
            // До входа статусы соединения не всплывают — их видно на карточке входа.
            if (App.statusMessage.length > 0 && App.authenticated) {
                toast.isError = false
                toast.opacity = 1
                toastTimer.restart()
            }
        }
    }
}
