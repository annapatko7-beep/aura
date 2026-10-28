import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  DesktopShell — оболочка после входа в дизайне 2026 (как AppShell в вебе):
  боковая панель на сплошном фоне — знак Selfvoy, кнопка «Создать» с быстрыми
  действиями, разделы (Сегодня, Запись, Абонементы, Ассистент, Переговоры,
  Задачи, Контакты, Входящие), внизу — Настройки и профиль с тарифом. Справа —
  текущий раздел. В узком окне панель сворачивается в рельс из иконок.

  navigate("раздел") или navigate("раздел:действие") — из любой страницы;
  действие передаётся странице через runAction(action), если она его умеет
  (например, «booking:post» — пост со свободными окнами).
*/
Item {
    id: shell

    property string current: "home"
    property string pendingAction: ""
    readonly property bool wide: width >= SelfvoyTheme.breakpointWide
    readonly property var planInfo: App ? F.planView(App.plan) : null
    readonly property int inboxCount: App ? App.unreadNotifications : 0

    readonly property var sections: [
        { key: "home",          label: "Сегодня",     icon: "calendar-days" },
        { key: "booking",       label: "Запись",      icon: "calendar-clock" },
        { key: "packages",      label: "Абонементы",  icon: "ticket" },
        { key: "selfvoy",       label: "Ассистент",   icon: "sparkles" },
        { key: "negotiations",  label: "Переговоры",  icon: "handshake" },
        { key: "tasks",         label: "Задачи",      icon: "square-check" },
        { key: "contacts",      label: "Контакты",    icon: "users" }
    ]

    readonly property var quickActions: [
        { to: "booking:new", label: "Новая страница записи", hint: "Ссылка, по которой клиенты записываются сами", icon: "calendar-clock", tone: SelfvoyTheme.accentSolid },
        { to: "booking:post", label: "Пост со свободными окнами", hint: "Готовый текст для Telegram или VK", icon: "megaphone", tone: SelfvoyTheme.amber },
        { to: "packages:new", label: "Выдать абонемент", hint: "Пакет занятий клиенту — со ссылкой для записи", icon: "ticket", tone: SelfvoyTheme.capsule[3] },
        { to: "negotiations", label: "Договориться о встрече", hint: "Selfvoy согласует время с другим Selfvoy", icon: "handshake", tone: SelfvoyTheme.capsule[1] },
        { to: "tasks:new", label: "Новая задача", hint: "Напоминание или дело со сроком", icon: "square-check", tone: SelfvoyTheme.capsule[4] },
        { to: "selfvoy", label: "Спросить ассистента", hint: "Текстом или голосом", icon: "sparkles", tone: "#636366" }
    ]

    function navigate(target) {
        const parts = String(target).split(":")
        const pageKey = parts[0]
        const action = parts.length > 1 ? parts[1] : ""
        if (App) {
            if (pageKey === "settings") { App.loadPermissions(); App.loadIntegrations(); App.loadReferral() }
            else if (pageKey === "booking" || pageKey === "home") App.loadBooking()
            else if (pageKey === "packages") { App.loadPackages(""); App.loadBooking() }
            else if (pageKey === "confirmations") App.loadConfirmations()
            else if (pageKey === "notifications") { App.loadNotifications(); App.loadPushDevices() }
            else if (pageKey === "tasks") App.loadTasks()
        }
        if (current === pageKey) {
            if (action && page.item && typeof page.item.runAction === "function") page.item.runAction(action)
            return
        }
        pendingAction = action
        current = pageKey
    }

    // ------------------------------------------------------------- пункт меню
    component NavItem: Item {
        id: item
        property string key: ""
        property string label: ""
        property string icon: ""
        property int badge: 0
        property color badgeColor: SelfvoyTheme.accentSolid
        readonly property bool active: shell.current === key

        Layout.fillWidth: true
        implicitHeight: 38

        Rectangle {
            anchors.fill: parent
            radius: SelfvoyTheme.radiusSmall
            color: item.active ? SelfvoyTheme.accentTint : (area.containsMouse ? SelfvoyTheme.field : "transparent")
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: shell.wide ? 12 : 0
            anchors.rightMargin: shell.wide ? 10 : 0
            spacing: 10

            Item { Layout.fillWidth: !shell.wide }
            Icon {
                name: item.icon
                size: 18
                color: item.active ? SelfvoyTheme.accent : SelfvoyTheme.text2
            }
            Text {
                visible: shell.wide
                Layout.fillWidth: true
                text: item.label
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: SelfvoyTheme.fontBody - 1
                font.weight: Font.Medium
                color: item.active ? SelfvoyTheme.accent : SelfvoyTheme.text
                elide: Text.ElideRight
            }
            Rectangle {
                visible: item.badge > 0 && shell.wide
                implicitWidth: Math.max(20, badgeText.implicitWidth + 12)
                implicitHeight: 20
                radius: 10
                color: item.badgeColor
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: item.badge
                    font.family: SelfvoyTheme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    color: item.badgeColor === SelfvoyTheme.amber ? "#1A1400" : "#FFFFFF"
                }
            }
            Item { Layout.fillWidth: !shell.wide }
        }

        Rectangle {
            visible: item.badge > 0 && !shell.wide
            width: 8; height: 8; radius: 4
            color: item.badgeColor
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 6
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: shell.navigate(item.key)
        }

        ToolTip.visible: !shell.wide && area.containsMouse
        ToolTip.delay: 400
        ToolTip.text: item.label + (item.badge > 0 ? ": " + item.badge : "")
    }

    // ------------------------------------------------------------- раскладка
    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: sidebar
            Layout.preferredWidth: shell.wide ? SelfvoyTheme.sidebarWidth : 72
            Layout.fillHeight: true
            color: SelfvoyTheme.bgElev

            Rectangle {
                anchors.right: parent.right
                width: 1; height: parent.height
                color: SelfvoyTheme.border
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 22
                anchors.bottomMargin: 16
                anchors.leftMargin: shell.wide ? 14 : 10
                anchors.rightMargin: shell.wide ? 14 : 10
                spacing: 2

                // Знак и имя
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: shell.wide ? 10 : 0
                    Layout.bottomMargin: 16
                    spacing: 10
                    Item { Layout.fillWidth: !shell.wide }
                    SelfvoyLogo { size: 26 }
                    Text {
                        visible: shell.wide
                        Layout.fillWidth: true
                        text: "Selfvoy"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.Bold
                        font.letterSpacing: -0.3
                        color: SelfvoyTheme.text
                    }
                    Item { Layout.fillWidth: !shell.wide }
                }

                GlassButton {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 12
                    variant: "accent"
                    symbol: "plus"
                    text: shell.wide ? "Создать" : ""
                    implicitHeight: 42
                    ToolTip.visible: !shell.wide && hovered
                    ToolTip.text: "Создать"
                    onClicked: quick.open()
                }

                Repeater {
                    model: shell.sections
                    delegate: NavItem {
                        required property var modelData
                        key: modelData.key
                        label: modelData.label
                        icon: modelData.icon
                        badge: modelData.key === "negotiations" && App && App.negotiation.accepted
                               && !App.negotiation.booking ? 1 : 0
                    }
                }
                NavItem {
                    key: "notifications"
                    label: "Входящие"
                    icon: "bell"
                    badge: shell.inboxCount
                }
                // Барьер безопасности: опасные действия ассистента ждут решения.
                NavItem {
                    visible: App && App.confirmations.length > 0
                    key: "confirmations"
                    label: "Подтверждения"
                    icon: "triangle-alert"
                    badge: App ? App.confirmations.length : 0
                    badgeColor: SelfvoyTheme.amber
                }

                Item { Layout.fillHeight: true }

                Rectangle { Layout.fillWidth: true; Layout.bottomMargin: 10; height: 1; color: SelfvoyTheme.border }

                NavItem { key: "settings"; label: "Настройки"; icon: "settings" }

                // Профиль и тариф
                Item {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    implicitHeight: 54

                    Rectangle {
                        anchors.fill: parent
                        radius: SelfvoyTheme.radiusField
                        color: meArea.containsMouse ? SelfvoyTheme.field : "transparent"
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: shell.wide ? 8 : 0
                        anchors.rightMargin: shell.wide ? 8 : 0
                        spacing: 10
                        Item { Layout.fillWidth: !shell.wide }
                        SelfvoyAvatar {
                            name: App ? App.userName : ""
                            size: 34
                            online: App ? App.connected : false
                        }
                        ColumnLayout {
                            visible: shell.wide
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: App ? App.userName : ""
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall + 1
                                font.weight: Font.DemiBold
                                color: SelfvoyTheme.text
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: !App || !App.connected ? "нет связи с сервером"
                                      : shell.planInfo ? shell.planInfo.label : (App.userEmail || "")
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontMicro
                                color: App && App.connected ? SelfvoyTheme.text2 : SelfvoyTheme.warning
                                elide: Text.ElideRight
                            }
                        }
                        ProBadge { visible: shell.wide && !!shell.planInfo && shell.planInfo.pro }
                        Item { Layout.fillWidth: !shell.wide }
                    }
                    MouseArea {
                        id: meArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: shell.navigate("settings")
                    }
                }
            }
        }

        Item {
            id: main
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: page
                anchors.fill: parent
                sourceComponent: {
                    switch (shell.current) {
                    case "selfvoy": return selfvoyPage
                    case "negotiations": return negotiationsPage
                    case "tasks": return tasksPage
                    case "contacts": return contactsPage
                    case "settings": return settingsPage
                    case "booking": return bookingPage
                    case "packages": return packagesPage
                    case "confirmations": return confirmationsPage
                    case "notifications": return notificationsPage
                    default: return homePage
                    }
                }
                onLoaded: {
                    fade.restart()
                    if (shell.pendingAction && item && typeof item.runAction === "function")
                        Qt.callLater(item.runAction, shell.pendingAction)
                    shell.pendingAction = ""
                }
                NumberAnimation { id: fade; target: page.item; property: "opacity"; from: 0; to: 1; duration: SelfvoyTheme.moveMs }
            }

            Connections {
                target: page.item
                ignoreUnknownSignals: true
                function onNavigate(to) { shell.navigate(to) }
            }
        }
    }

    // ------------------------------------------------------------- «Создать»
    SheetPopup {
        id: quick
        title: "Создать"
        sheetWidth: 480
        Group {
            Layout.fillWidth: true
            Repeater {
                model: shell.quickActions
                delegate: Cell {
                    required property var modelData
                    required property int index
                    divider: index > 0
                    clickable: true
                    icon: modelData.icon
                    iconColor: modelData.tone
                    label: modelData.label
                    hint: modelData.hint
                    onClicked: { quick.close(); shell.navigate(modelData.to) }
                }
            }
        }
    }

    // ------------------------------------------------------------- «Нужен Pro»
    SheetPopup {
        id: proSheet
        property string message: F.PRO_REQUIRED_TEXT
        title: "Доступно на тарифе Pro"
        sheetWidth: 460
        leading: Component { ProBadge { scale: 1.4 } }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: proSheet.message
            font.family: SelfvoyTheme.fontFamily
            font.pixelSize: SelfvoyTheme.fontBody
            color: SelfvoyTheme.text
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: shell.planInfo ? "Сейчас: " + shell.planInfo.label + ". " + shell.planInfo.hint : ""
            visible: text.length > 0
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
                symbol: "gift"
                text: "Пригласить коллегу"
                onClicked: { proSheet.close(); shell.navigate("settings:referral") }
            }
            GlassButton { variant: "quiet"; text: "Понятно"; onClicked: proSheet.close() }
        }
    }

    Connections {
        target: App
        function onProRequired(message) {
            proSheet.message = message
            if (!proSheet.opened) proSheet.open()
        }
    }

    Component { id: homePage; HomePage {} }
    Component { id: selfvoyPage; MySelfvoyPage {} }
    Component { id: negotiationsPage; NegotiationsPage {} }
    Component { id: tasksPage; TasksPage {} }
    Component { id: contactsPage; ContactsPage {} }
    Component { id: settingsPage; SettingsPage { embedded: true } }
    Component { id: bookingPage; BookingPage {} }
    Component { id: packagesPage; PackagesPage {} }
    Component { id: confirmationsPage; InboxPage { mode: "confirmations" } }
    Component { id: notificationsPage; InboxPage { mode: "notifications" } }
}
