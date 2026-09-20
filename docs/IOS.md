# Aura для iOS (этап 10)

Нативный клиент на Swift/SwiftUI. Работает по тому же WS-протоколу, что
Qt-клиент и e2e (см. PROTOCOL.md): кадр `{id, type, payload}`, ответы
`ok`/`error`, события `{"type":"event","event":...}`.

## Состав

```
ios/
  project.yml                 # XcodeGen: приложение Aura + AuraTests (iOS 17+)
  AuraKit/                    # общий Swift-пакет (транспорт/протокол/модели)
    Sources/AuraKit/
      JSONValue.swift         # динамический JSON payloads
      Protocol.swift          # кадры протокола, AuraError, имена событий
      WebSocketTransport.swift# URLSessionWebSocketTask: id-мэтчинг, AsyncStream событий
      AuraClient.swift        # типизированный API (auth/chat/agent/tasks/permissions/integrations)
      KeychainStore.swift     # токены/сервер/device_id — только Keychain
      DeepLink.swift          # разбор aura://-ссылок
    Tests/AuraKitTests/       # JSON, кадры, deep links (swift test без симулятора)
  Aura/
    AuraApp.swift             # @main: bootstrap, onOpenURL, STT-fallback
    Support/AppStore.swift    # состояние UI, события chat.message/task.due
    Support/SpeechService.swift   # гибрид: SFSpeechRecognizer + серверный STT; AVSpeechSynthesizer
    Support/EventKitService.swift # экспорт задач в напоминания iOS (по кнопке)
    Intents/                  # App Intents: «Спросить Ауру», «Создать задачу», фразы Siri
    Views/                    # вход/2FA, чаты, задачи, подтверждения, настройки,
                              # разрешения, интеграции, Quick Actions, голос
  AuraTests/                  # смоук-тесты UI-слоя
```

## Возможности (UI + эндпоинты + ошибки)

| фича | UI | эндпоинты |
| --- | --- | --- |
| Вход/регистрация, email-код, 2FA (TOTP + доверенное устройство) | LoginView | `auth.register/verifyEmail/login/login2fa/token/refresh/logout` |
| Чаты и сообщения | ChatsView/ChatView | `chat.list/open/history/send`, событие `chat.message` |
| «Спросить Ауру» | переключатель в чате, голос, Siri | `agent.ask` (в чате — с контекстом, Siri — `chat_id=0`) |
| Задачи и напоминания | TasksView | `tasks.list/create/complete/cancel/delete`, событие `task.due` (озвучивается) |
| Экспорт задач в напоминания iOS | кнопка в задаче | EventKit (`requestFullAccessToReminders` — только по нажатию) |
| Барьер подтверждения | ConfirmationsView (badge) | `confirmation.list/approve/deny` |
| Разрешения инструментов | PermissionsView | `permissions.list/set` |
| Интеграции Google | IntegrationsView | `integrations.list/begin/callback/revoke/sync`; consent в Safari → возврат `aura://oauth?provider&code&state` |
| Голос | VoiceOverlay, Siri | on-device SFSpeechRecognizer; fallback `speech.transcribe` (WAV 16k mono, base64); озвучка AVSpeechSynthesizer |

Ошибки сервера (`code`/`message`) показываются алертом; транспортные ошибки —
в статусной строке. Состояния: `disconnected/connecting/connected`,
`loggedOut/needsEmailCode/needs2fa/loggedIn`, голос `idle/listening/processing`.

## Siri, Команды, Back Tap

- App Intents: **AskAuraIntent** (`agent.ask`, `chat_id=0`, ответ голосом
  через `ProvidesDialog`) и **CreateTaskIntent** (`tasks.create` + ISO-дата
  напоминания). Выполняются без открытия приложения.
- **IntentSession** — отдельный ленивый WS-клиент для intent'ов: берёт
  сервер и access-токен из Keychain, аутентифицируется через `auth.token`.
- Фразы Siri (AppShortcutsProvider): «Спросить Ауру в Aura: …», «Создать
  задачу в Aura: …».
- **Back Tap напрямую сторонним приложениям недоступен** (системный жест).
  Приложение его не имитирует: в QuickActionsView — пошаговая инструкция,
  как назначить жест на команду «Спросить Ауру» (Универсальный доступ →
  Касание → Касание сзади → Команды).

## Deep links (`aura://`)

`aura://chats/{id}`, `aura://voice`, `aura://ask?text=…`, `aura://tasks`,
`aura://settings`, `aura://oauth?provider&code&state` (возврат из Google
OAuth — клиент сам передаёт code/state в `integrations.callback`).
В продакшене схему дополняют Universal Links — роутинг тот же (DeepLink.swift).

## Безопасность

- Access/refresh-токены, адрес сервера и device_id — только в Keychain
  (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), не в UserDefaults.
- Токены интеграций на устройство не попадают вовсе (см. SECURITY.md §9).
- Разрешения микрофона/речи/напоминаний запрашиваются в момент действия;
  фоновых режимов и push на этом этапе нет (push — этап 13, APNs).

## Сборка и тесты

```bash
brew install xcodegen && cd ios && xcodegen generate && open Aura.xcodeproj
cd ios/AuraKit && swift test        # тесты общего слоя без симулятора
```

Автосверка протокола без macOS (ловит опечатки в именах хендлеров):

```bash
python3 tools/check_ios_protocol.py
# хендлеров сервера: 59; типов в iOS-клиенте: 33 — все известны серверу
```

## Ограничения этапа

- Компиляция Swift и симулятор недоступны в песочнице разработки: код
  проверен сверкой протокола, валидацией project.yml (YAML), балансом
  синтаксиса и ревью; `swift test`/Xcode-сборку нужно прогнать на Mac.
- Universal Links, APNs-push, Live Activities, виджет и Share Extension —
  этап 13. EventKit-календарь (события, не напоминания) — по потребности.
