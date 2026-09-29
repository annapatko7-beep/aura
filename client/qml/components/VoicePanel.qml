import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aura

/*
  VoicePanel — голосовой ввод: запись с микрофона → распознавание → текст.

  Состояния: idle → listening → processing → success/error.
  Весь захват аудио, упаковка в WAV и распознавание живут в C++ (AppStore);
  QML лишь отражает состояние (App.voiceState/voiceLevel/transcript/micAvailable)
  и вызывает слоты (startListening/stopListening/cancelListening/sendTranscript).

  Поле расшифровки редактируется перед отправкой; оно же — текстовый фолбэк,
  если микрофон недоступен. Есть автоотправка и отмена записи.
*/
GlassPanel {
    id: panel

    readonly property string vState: App ? App.voiceState : "idle"
    readonly property bool listening: vState === "listening"
    readonly property bool processing: vState === "processing"
    readonly property bool micOk: App ? App.micAvailable : true

    padding: AuraTheme.spaceMd

    // Синхронизация поля с расшифровкой из C++ — императивно, без привязки,
    // чтобы правка пользователем не воевала с биндингом (иначе цикл).
    Connections {
        target: App
        function onTranscriptChanged() {
            transcriptField.text = App ? App.transcript : ""
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: AuraTheme.spaceSm

        // ------------------------------------------------ индикатор записи
        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceMd
            visible: panel.listening || panel.processing

            // «Волна» из столбиков, реагирующих на уровень звука.
            Item {
                Layout.preferredWidth: 56
                Layout.preferredHeight: 26
                visible: panel.listening
                Repeater {
                    model: 5
                    Rectangle {
                        required property int index
                        readonly property real lvl: App ? App.voiceLevel : 0
                        width: 4
                        radius: 2
                        color: AuraTheme.accent
                        x: index * 13
                        anchors.verticalCenter: parent.verticalCenter
                        height: 5 + lvl * (20 - Math.abs(index - 2) * 4)
                        opacity: 0.55 + lvl * 0.45
                        Behavior on height { NumberAnimation { duration: 90 } }
                    }
                }
            }

            // «Спиннер» на время распознавания.
            Rectangle {
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 11
                visible: panel.processing
                color: "transparent"
                border.width: 2
                border.color: AuraTheme.accentA(0.3)
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: parent.height
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: AuraTheme.accent
                    // Дуга: прозрачная половина, вращаем.
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: AuraTheme.bgRaised
                        opacity: 0.001
                    }
                }
                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                    running: panel.processing
                }
            }

            Text {
                Layout.fillWidth: true
                text: panel.processing ? "Распознаю речь…" : "Слушаю… говорите"
                font.family: AuraTheme.fontFamily
                font.pixelSize: AuraTheme.fontSmall
                color: AuraTheme.textSecondary
            }

            GlassButton {
                variant: "danger"
                glyph: "✕"
                text: ""
                implicitWidth: 40
                implicitHeight: 40
                onClicked: App.cancelListening()
            }
        }

        // --------------------------------------- микрофон + расшифровка + отправка
        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceSm

            GlassButton {
                id: micButton
                variant: panel.listening ? "danger" : "accent"
                glyph: panel.listening ? "■" : "●"
                text: panel.listening ? "Стоп" : (panel.processing ? "…" : "Голосом")
                enabled: !panel.processing
                implicitHeight: 48
                onClicked: panel.listening ? App.stopListening() : App.startListening()
            }

            GlassField {
                id: transcriptField
                Layout.fillWidth: true
                implicitHeight: 48
                placeholderText: panel.micOk
                                 ? "Нажмите «Голосом» и говорите…"
                                 : "Микрофон недоступен — введите текст"
                enabled: !panel.processing
                onAccepted: sendTranscriptButton.clicked()
            }

            GlassButton {
                id: sendTranscriptButton
                glyph: "➤"
                text: ""
                implicitWidth: 52
                implicitHeight: 48
                enabled: transcriptField.text.trim().length > 0 && !panel.processing
                onClicked: {
                    App.setTranscript(transcriptField.text)
                    App.sendTranscript()
                }
            }
        }

        // --------------------------------------------- ошибка / текстовый фолбэк
        Text {
            Layout.fillWidth: true
            visible: !panel.micOk || panel.vState === "error"
            text: !panel.micOk
                  ? "Микрофон недоступен — сообщение можно ввести текстом."
                  : (App && App.errorMessage ? App.errorMessage : "Не удалось распознать речь")
            font.family: AuraTheme.fontFamily
            font.pixelSize: AuraTheme.fontMicro
            color: AuraTheme.danger
            wrapMode: Text.WordWrap
        }

        // --------------------------------------------------- автоотправка
        RowLayout {
            Layout.fillWidth: true
            spacing: AuraTheme.spaceSm

            Item { Layout.fillWidth: true }

            GlassButton {
                variant: (App && App.autoSendVoice) ? "accent" : "quiet"
                text: "Автоотправка: " + (App && App.autoSendVoice ? "вкл" : "выкл")
                implicitHeight: 36
                onClicked: App.setAutoSendVoice(!(App && App.autoSendVoice))
            }
        }
    }
}
