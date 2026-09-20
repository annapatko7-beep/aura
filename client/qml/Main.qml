import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import Aura

/*
  Главное окно Qt-клиента Aura.

  Фон — тёмный графит с двумя размытыми «сияниями»; он же служит backdrop'ом
  для всех стеклянных панелей (GlassSurface размывает именно его).
*/
ApplicationWindow {
    id: window

    width: 1120
    height: 780
    minimumWidth: 420
    minimumHeight: 640
    visible: true
    color: AuraTheme.bgDeep
    title: App && App.authenticated ? "Aura — " + App.userName : "Aura"

    // ------------------------------------------------------------ фон
    Item {
        id: backdrop
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: AuraTheme.bgBase }
                GradientStop { position: 0.55; color: AuraTheme.bgDeep }
                GradientStop { position: 1.0; color: "#050607" }
            }
        }

        // Тонкая сетка — едва заметная текстура
        Canvas {
            id: grid
            anchors.fill: parent
            opacity: 0.35
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.strokeStyle = "rgba(255,255,255,0.03)"
                ctx.lineWidth = 1
                for (let x = 0; x < width; x += 44) {
                    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke()
                }
                for (let y = 0; y < height; y += 44) {
                    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke()
                }
            }
            Component.onCompleted: requestPaint()
        }

        // Два размытых пятна «северного сияния»
        Rectangle {
            id: blobA
            width: 520
            height: 520
            radius: width / 2
            x: -120
            y: -140
            color: "#4C5BD6"
            opacity: 0.30
            layer.enabled: true
            layer.effect: MultiEffect {
                blurEnabled: true
                blurMax: 160
                blur: 1.0
            }
            SequentialAnimation on x {
                loops: Animation.Infinite
                NumberAnimation { to: 120; duration: 22000; easing.type: Easing.InOutSine }
                NumberAnimation { to: -120; duration: 22000; easing.type: Easing.InOutSine }
            }
        }

        Rectangle {
            id: blobB
            width: 460
            height: 460
            radius: width / 2
            x: parent.width - 260
            y: parent.height - 300
            color: "#2FA9A0"
            opacity: 0.22
            layer.enabled: true
            layer.effect: MultiEffect {
                blurEnabled: true
                blurMax: 160
                blur: 1.0
            }
            SequentialAnimation on y {
                loops: Animation.Infinite
                NumberAnimation { to: 60; duration: 26000; easing.type: Easing.InOutSine }
                NumberAnimation { to: window.height - 300; duration: 26000; easing.type: Easing.InOutSine }
            }
        }
    }

    // -------------------------------------------------------- маршрутизация
    StackView {
        id: stack
        anchors.fill: parent
        initialItem: App && App.authenticated ? chatsPage : loginPage

        popEnter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AuraTheme.moveMs }
        }
        pushEnter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: AuraTheme.moveMs }
            NumberAnimation { property: "x"; from: 40; to: 0; duration: AuraTheme.moveMs; easing.type: Easing.OutCubic }
        }
    }

    Component {
        id: loginPage
        LoginPage {
            onLoggedIn: stack.replace(null, chatsPage)
        }
    }

    Component {
        id: chatsPage
        ChatsPage {
            onOpenChat: function(chatId) { stack.push(chatPage, { chatId: chatId }) }
            onOpenSettings: stack.push(settingsPage)
        }
    }

    Component {
        id: chatPage
        ChatPage {
            property int chatId: 0
            onBack: stack.pop()
            onOpenSettings: stack.push(settingsPage)
        }
    }

    Component {
        id: settingsPage
        SettingsPage {
            onBack: stack.pop()
            onLoggedOut: stack.replace(null, loginPage)
        }
    }

    // Смена состояния авторизации из C++ (например, токен истёк)
    Connections {
        target: App
        function onAuthenticatedChanged() {
            if (!App.authenticated && stack.currentItem !== null) {
                stack.replace(null, loginPage)
            }
        }
    }

    // ------------------------------------------------------------- тосты
    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: AuraTheme.spaceLg
        width: Math.min(parent.width - AuraTheme.spaceXl * 2, toastText.implicitWidth + AuraTheme.spaceLg * 2)
        height: 46
        radius: AuraTheme.radiusPill
        color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.14)
        opacity: 0
        visible: opacity > 0

        Behavior on opacity { NumberAnimation { duration: AuraTheme.moveMs } }

        Text {
            id: toastText
            anchors.centerIn: parent
            width: parent.width - AuraTheme.spaceLg * 2
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontSmall
            color: toast.isError ? AuraTheme.danger : AuraTheme.textPrimary
            text: toast.isError ? (App ? App.errorMessage : "") : (App ? App.statusMessage : "")
        }

        property bool isError: false
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
            if (App.statusMessage.length > 0) {
                toast.isError = false
                toast.opacity = 1
                toastTimer.restart()
            }
        }
    }
}
