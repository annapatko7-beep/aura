import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Selfvoy
import "Format.js" as F

/*
  ContactsPage — люди и их Selfvoy (макет Contacts).

  Слева — поиск, фильтры и список собеседников (из чатов; новые находятся
  через users.search и добавляются chat.open). Справа — карточка человека:
  последнее сообщение, что вашему Selfvoy разрешено делать, «Начать переговоры»
  (agent.negotiate) и «Открыть чат» — переписка открывается в этой же колонке.
*/
Item {
    id: page

    signal navigate(string to)

    readonly property var contacts: F.contactsFrom(App ? App.chats : [], App ? App.userId : 0)
    property string filter: "all"
    property string query: ""
    property var selected: null
    property bool chatOpen: false

    readonly property var visibleContacts: contacts.filter(c => {
        if (filter === "recent" && !c.lastAt) return false
        const q = query.trim().toLowerCase()
        return !q || c.name.toLowerCase().indexOf(q) >= 0 || (c.email || "").toLowerCase().indexOf(q) >= 0
    })
    // Найденные на сервере, которых ещё нет в контактах
    readonly property var found: (App ? App.userResults : []).filter(u =>
        u.id !== (App ? App.userId : 0) && !contacts.some(c => c.id === u.id))

    function permission(tool) {
        const item = (App ? App.toolPermissions : []).find(p => p.tool === tool)
        return item ? item.mode : ""
    }
    readonly property var modeLabel: ({ allow: "сам", ask: "с вашего подтверждения", deny: "запрещено" })

    onContactsChanged: if (!selected && contacts.length) selected = contacts[0]
    Component.onCompleted: if (App) App.loadPermissions()

    Timer {
        id: searchDelay
        interval: 300
        onTriggered: App.searchUsers(page.query)
    }

    // Декоративные сферы, как в макете
    component Sphere: Rectangle {
        radius: width / 2
        color: SelfvoyTheme.field
        border.width: 1
        border.color: SelfvoyTheme.glass(0.25)
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 36

        // ============================================================ список
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 18

            Text {
                text: "Контакты"
                font.family: SelfvoyTheme.fontFamily
                font.pixelSize: 38
                font.weight: Font.Bold
                color: SelfvoyTheme.text
            }

            // Поиск — подчёркнутая строка
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Icon { name: "search"; size: 18 }
                    TextField {
                        id: search
                        Layout.fillWidth: true
                        background: null
                        placeholderText: "Искать людей или Selfvoy"
                        placeholderTextColor: SelfvoyTheme.text3
                        color: SelfvoyTheme.text
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontLead
                        onTextChanged: { page.query = text; searchDelay.restart() }
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: SelfvoyTheme.glass(search.activeFocus ? 0.4 : 0.18) }
            }

            RowLayout {
                spacing: 10
                Repeater {
                    model: [{ k: "all", l: "Все" }, { k: "recent", l: "Недавние" }]
                    delegate: Chip {
                        required property var modelData
                        text: modelData.l
                        active: page.filter === modelData.k
                        onClicked: page.filter = modelData.k
                    }
                }
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: page.visibleContacts
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property bool active: page.selected && page.selected.id === modelData.id
                    readonly property bool inline: width >= 460
                    readonly property string preview: modelData.lastMessage ? modelData.lastMessage : "Selfvoy доступен"
                    width: list.width
                    height: 70
                    radius: 16
                    color: active ? SelfvoyTheme.accentA(0.12) : (rowArea.containsMouse ? SelfvoyTheme.glass(0.04) : "transparent")
                    border.width: active ? 1 : 0
                    border.color: SelfvoyTheme.lilacA(0.3)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 18
                        spacing: 16
                        SelfvoyAvatar { name: row.modelData.name; size: 46 }
                        // В узкой колонке превью уходит под имя: в одну строку
                        // оно сжималось в ноль и оставалась одна черта-разделитель.
                        ColumnLayout {
                            Layout.fillWidth: !row.inline
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.name
                                elide: Text.ElideRight
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontBody
                                font.weight: Font.Bold
                                color: SelfvoyTheme.text
                            }
                            Text {
                                visible: !row.inline
                                Layout.fillWidth: true
                                text: row.preview
                                elide: Text.ElideRight
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                color: SelfvoyTheme.text2
                            }
                        }
                        Rectangle { visible: row.inline; implicitWidth: 50; implicitHeight: 1; color: SelfvoyTheme.text3 }
                        Text {
                            visible: row.inline
                            Layout.fillWidth: true
                            text: row.preview
                            elide: Text.ElideRight
                            font.family: SelfvoyTheme.fontFamily
                            font.pixelSize: SelfvoyTheme.fontSmall
                            color: SelfvoyTheme.text2
                        }
                        Rectangle {
                            implicitWidth: 10; implicitHeight: 10; radius: 5
                            color: SelfvoyTheme.accentSoft  // «Selfvoy доступен»; онлайн-статус виден в чате
                        }
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { page.selected = row.modelData; page.chatOpen = false }
                    }
                }

                footer: ColumnLayout {
                    width: list.width
                    spacing: 6
                    Text {
                        visible: page.visibleContacts.length === 0 && page.found.length === 0
                        Layout.topMargin: 10
                        text: page.query ? "Никого не нашли" : "Пока нет собеседников. Найдите человека по имени или email."
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        color: SelfvoyTheme.text3
                    }
                    Text {
                        visible: page.found.length > 0
                        Layout.topMargin: 14
                        text: "Найдено в Selfvoy"
                        font.family: SelfvoyTheme.fontFamily
                        font.pixelSize: SelfvoyTheme.fontSmall
                        font.weight: Font.Bold
                        color: SelfvoyTheme.text2
                    }
                    Repeater {
                        model: page.found
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            spacing: 14
                            SelfvoyAvatar { name: modelData.display_name || "?"; size: 38 }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text { text: modelData.display_name || ""; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontBody; font.weight: Font.DemiBold; color: SelfvoyTheme.text }
                                // users.search не отдаёт email — только имя и «в сети».
                                Text { text: modelData.online ? "в сети" : "есть Selfvoy"; font.family: SelfvoyTheme.fontFamily; font.pixelSize: SelfvoyTheme.fontMicro; color: SelfvoyTheme.text3 }
                            }
                            GlassButton {
                                text: "Добавить"
                                symbol: "user-plus"
                                implicitHeight: 36
                                // chat.open принимает id пользователя строкой.
                                onClicked: { App.openChat(String(modelData.id)); search.text = "" }
                            }
                        }
                    }
                }
            }

            GlassButton {
                text: "Пригласить в Selfvoy"
                symbol: "user-plus"
                implicitHeight: 50
                leftPadding: 30
                rightPadding: 30
                ToolTip.visible: hovered
                ToolTip.text: "Найдите человека по email — если у него есть Selfvoy, он появится в списке"
                onClicked: search.forceActiveFocus()
            }
        }

        // ============================================================ карточка
        Item {
            Layout.preferredWidth: page.chatOpen ? Math.min(560, page.width * 0.5) : 340
            Layout.fillHeight: true

            // Сферы залезают в колонку списка — показываем, только пока она широкая.
            Sphere { width: 120; height: 120; x: -70; y: -20; visible: false }  // 2026: без декоративных сфер
            Sphere { width: 22; height: 22; x: -90; y: 150; visible: false }

            // Профиль
            Rectangle {
                id: profile
                visible: !page.chatOpen && page.selected !== null
                width: parent.width
                height: Math.min(parent.height, 1 + 33 + profileCol.implicitHeight + 22 + actions.implicitHeight + 26)
                radius: 30
                color: SelfvoyTheme.surface
                border.width: 1
                border.color: SelfvoyTheme.glass(0.28)

                // Если карточка не влезает по высоте (узкое/низкое окно), информация
                // прокручивается внутри неё, а не выталкивает кнопки за край окна.
                Flickable {
                    id: profileScroll
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: actions.top
                    anchors.topMargin: 1
                    anchors.bottomMargin: 22
                    clip: true
                    contentWidth: width
                    contentHeight: profileCol.implicitHeight + 33
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    ColumnLayout {
                        id: profileCol
                        x: 26
                        y: 33
                        width: profileScroll.width - 52
                        spacing: 22

                        ColumnLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 4
                            SelfvoyAvatar {
                                Layout.alignment: Qt.AlignHCenter
                                name: page.selected ? page.selected.name : ""
                                size: 100
                                ring: true
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: 12
                                text: page.selected ? page.selected.name : ""
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontTitle
                                font.weight: Font.Bold
                                color: SelfvoyTheme.text
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: "Selfvoy доступен · " + (page.selected ? page.selected.email : "")
                                font.family: SelfvoyTheme.fontFamily
                                font.pixelSize: SelfvoyTheme.fontSmall
                                color: SelfvoyTheme.text2
                            }
                        }

                        component InfoBlock: ColumnLayout {
                            id: block
                            property string icon: ""
                            property string title: ""
                            property var lines: []
                            Layout.fillWidth: true
                            spacing: 8
                            RowLayout {
                                spacing: 10
                                Rectangle {
                                    implicitWidth: 28; implicitHeight: 28; radius: 14
                                    color: SelfvoyTheme.accent
                                    Icon { anchors.centerIn: parent; name: block.icon; size: 14; color: "#FFFFFF" }
                                }
                                Text {
                                    text: block.title
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontBody
                                    font.weight: Font.Bold
                                    color: SelfvoyTheme.text
                                }
                            }
                            Repeater {
                                model: block.lines
                                delegate: Text {
                                    required property string modelData
                                    Layout.leftMargin: 38
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: modelData
                                    font.family: SelfvoyTheme.fontFamily
                                    font.pixelSize: SelfvoyTheme.fontSmall + 1
                                    font.weight: Font.DemiBold
                                    color: SelfvoyTheme.text
                                }
                            }
                        }

                        InfoBlock {
                            icon: "calendar-days"
                            title: "Последнее в переписке"
                            lines: page.selected && page.selected.lastMessage
                                   ? ["«" + page.selected.lastMessage + "»", F.dayPhrase(page.selected.lastAt) + ", " + F.time(page.selected.lastAt)]
                                   : ["Пока ничего"]
                        }
                        InfoBlock {
                            icon: "lock"
                            title: "Что может ваш Selfvoy"
                            lines: [
                                "Писать: " + (page.modeLabel[page.permission("send_message")] || "—"),
                                "Бронировать: " + (page.modeLabel[page.permission("book_table")] || "—"),
                                "Делиться: только свободными окнами"
                            ]
                        }
                    }
                }

                // Кнопки закреплены внизу карточки — всегда на виду.
                ColumnLayout {
                    id: actions
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 26
                    anchors.rightMargin: 26
                    anchors.bottomMargin: 26
                    spacing: 10
                    GlassButton {
                        Layout.fillWidth: true
                        implicitHeight: 50
                        text: "Начать переговоры"
                        symbol: "handshake"
                        busy: App ? App.busy : false
                        onClicked: {
                            App.negotiateAgent(page.selected.email, "", 60, F.windowHours("weekend"))
                            page.navigate("negotiations")
                        }
                    }
                    GlassButton {
                        Layout.fillWidth: true
                        implicitHeight: 46
                        variant: "quiet"
                        text: "Открыть чат"
                        symbol: "message-circle"
                        onClicked: page.chatOpen = true
                    }
                }
            }

            // Переписка
            GlassPanel {
                anchors.fill: parent
                visible: page.chatOpen && page.selected !== null
                radius: 28
                padding: 0
                backdrop: null
                base: 0.55
                ChatPage {
                    anchors.fill: parent
                    embedded: true
                    chatId: page.chatOpen && page.selected ? page.selected.chatId : 0
                    onBack: page.chatOpen = false
                }
            }
        }
    }
}
