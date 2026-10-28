# Selfvoy для iOS (этап 10)

Нативный клиент на Swift/SwiftUI. Работает по тому же WS-протоколу, что
Qt-клиент и e2e (см. PROTOCOL.md): кадр `{id, type, payload}`, ответы
`ok`/`error`, события `{"type":"event","event":...}`.

## Состав

```
ios/
  project.yml                 # XcodeGen: приложение Selfvoy + SelfvoyTests (iOS 17+)
  SelfvoyKit/                    # общий Swift-пакет (транспорт/протокол/модели)
    Sources/SelfvoyKit/
      JSONValue.swift         # динамический JSON payloads
      Protocol.swift          # кадры протокола, SelfvoyError, имена событий
      WebSocketTransport.swift# URLSessionWebSocketTask: id-мэтчинг, AsyncStream событий
      SelfvoyClient.swift        # типизированный API (auth/chat/agent/tasks/permissions/integrations)
      KeychainStore.swift     # токены/сервер/device_id — только Keychain
      DeepLink.swift          # разбор selfvoy://-ссылок
    Tests/SelfvoyKitTests/       # JSON, кадры, deep links (swift test без симулятора)
  Selfvoy/
    SelfvoyApp.swift             # @main: bootstrap, onOpenURL, STT-fallback
    Support/AppStore.swift    # состояние UI: диалог с Selfvoy, переговоры, контакты,
                              # задачи, события chat.message/task.due/notification.new
    Support/Theme2026.swift   # токены дизайна 2026 (Ink): цвета, радиусы, системный шрифт
    Support/Format.swift      # даты и подписи по-русски (сегодня/завтра, склонения)
    Support/SpeechService.swift   # гибрид: SFSpeechRecognizer + серверный STT; AVSpeechSynthesizer
    Support/EventKitService.swift # экспорт задач в напоминания iOS (по кнопке)
    Intents/                  # App Intents: «Спросить Selfvoy», «Создать задачу», фразы Siri
    Views/                    # v3 по макетам: Root (вкладки), Home, MySelfvoy,
                              # Negotiations, Tasks, Contacts (+чат), Settings,
                              # Confirmations, Notifications, Login, Onboarding,
                              # VoiceOverlay, Quick Actions; Components — общие элементы
  SelfvoyTests/                  # смоук-тесты UI-слоя
```

## Возможности (UI + эндпоинты + ошибки)

| фича | UI | эндпоинты |
| --- | --- | --- |
| Вход/регистрация, email-код, 2FA (TOTP + доверенное устройство) | LoginView | `auth.register/verifyEmail/login/login2fa/token/refresh/logout` |
| Сброс пароля | LoginView | `auth.forgotPassword/resetPassword` |
| Главная | HomeView | сводка задач, подтверждений, переговоров и уведомлений |
| «Мой Selfvoy» | MySelfvoyView, голос, Siri | `agent.ask` (`chat_id=0`); действия с `requires_confirmation` — кнопки «Подтвердить / Отклонить» под ответом; `memory.list` |
| Переговоры Аур | NegotiationsView | `agent.negotiate` (один раунд, до 5 подряд); бронь по кнопке — `tool.run` `book_table` + задача с напоминанием за 30 минут |
| Контакты и чаты | ContactsView/ChatView | `chat.list/open/history/send/read`, `users.search`, событие `chat.message`; в чате режим «Спросить Selfvoy» — `agent.ask` с `chat_id` |
| Задачи и напоминания | TasksView | `tasks.list/create(due_at)/complete/reopen/delete`, событие `task.due` (озвучивается) |
| Экспорт задач в напоминания iOS | долгое нажатие на задачу | EventKit (`requestFullAccessToReminders` — только по нажатию) |
| Барьер подтверждения | ConfirmationsView (badge) | `confirmation.list/approve/deny` |
| Разрешения инструментов | SettingsView | `permissions.list/set` |
| 2FA | SettingsView | `auth.status2fa/setup2fa/confirm2fa/disable2fa` |
| Интеграции Google | SettingsView | `integrations.list/begin/callback/revoke/sync`; consent в Safari → возврат `selfvoy://oauth?provider&code&state` |
| Уведомления (этап 13) | NotificationsView (из шапки «Главной», счётчик) | `notifications.list/read`, `devices.push.register/list/revoke`, событие `notification.new`; APNs-токен из AppDelegate |
| Голос | VoiceOverlay, Siri | on-device SFSpeechRecognizer; fallback `speech.transcribe` (WAV 16k mono, base64); озвучка AVSpeechSynthesizer |

Ошибки сервера (`code`/`message`) и статусы показываются плашкой сверху.
Состояния: `disconnected/connecting/connected`,
`loggedOut/needsEmailCode/needs2fa/needsResetCode/loggedIn`, голос
`idle/listening/processing`. Сессия восстанавливается при запуске по
access-токену (поле `token` ответа входа), при истёкшем — через `auth.refresh`.

## Siri, Команды, Back Tap

- App Intents: **AskSelfvoyIntent** (`agent.ask`, `chat_id=0`, ответ голосом
  через `ProvidesDialog`) и **CreateTaskIntent** (`tasks.create` + ISO-дата
  напоминания). Выполняются без открытия приложения.
- **IntentSession** — отдельный ленивый WS-клиент для intent'ов: берёт
  сервер и access-токен из Keychain, аутентифицируется через `auth.token`.
- Фразы Siri (AppShortcutsProvider): «Спросить Selfvoy в Selfvoy: …», «Создать
  задачу в Selfvoy: …».
- **Back Tap напрямую сторонним приложениям недоступен** (системный жест).
  Приложение его не имитирует: в QuickActionsView — пошаговая инструкция,
  как назначить жест на команду «Спросить Selfvoy» (Универсальный доступ →
  Касание → Касание сзади → Команды).

## Deep links (`selfvoy://`)

`selfvoy://chats/{id}`, `selfvoy://voice`, `selfvoy://ask?text=…`, `selfvoy://tasks`,
`selfvoy://settings`, `selfvoy://oauth?provider&code&state` (возврат из Google
OAuth — клиент сам передаёт code/state в `integrations.callback`),
`selfvoy://signup?ref=CODE` (регистрация с кодом приглашения).
В продакшене схему дополняют Universal Links — роутинг тот же (DeepLink.swift).

## Безопасность

- Access/refresh-токены, адрес сервера и device_id — только в Keychain
  (`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`), не в UserDefaults.
- Токены интеграций на устройство не попадают вовсе (см. SECURITY.md §9).
- Разрешения микрофона/речи/напоминаний запрашиваются в момент действия.
- Push (этап 13): APNs-токен получается через `registerForRemoteNotifications`
  (разрешение `UNUserNotificationCenter` запрашивается при запуске) и
  регистрируется на сервере (`devices.push.register`, hex без разделителей);
  entitlement `aps-environment` + фоновый режим `remote-notification` — в
  project.yml. Токен серверу отдаётся, но обратно клиенту не возвращается
  никогда (`devices.push.list` без поля `token`).

## Сборка и тесты

```bash
brew install xcodegen && cd ios && xcodegen generate && open Selfvoy.xcodeproj
cd ios/SelfvoyKit && swift test        # тесты общего слоя без симулятора
```

CI: `.github/workflows/build-ios.yml` (macOS 15, последний стабильный Xcode 16+; свежий XcodeGen пишет проект в его формате) генерирует проект,
гоняет `swift test` пакета SelfvoyKit, собирает приложение под симулятор и
запускает SelfvoyTests; Selfvoy.app для симулятора — в артефактах запуска.

Автосверка протокола без macOS (ловит опечатки в именах хендлеров):

```bash
python3 tools/check_ios_protocol.py
# хендлеров сервера: 64; типов в iOS-клиенте: 48 — все известны серверу
```

## Ограничения этапа

- Xcode и симулятор недоступны в песочнице разработки. Редизайн v3
  проверен компилятором Swift 5.10 под Linux: синтаксис всех файлов, типы
  SelfvoyKit, AppStore и Format, а экраны — против заглушки SwiftUI (строже
  настоящей: не больше 10 элементов в блоке); логика дат и склонений
  прогнана на значениях. Окончательная сборка — в CI «Build iOS» или в Xcode.
- Push-доставка требует внешнего шлюза APNs (сервер отдаёт готовый конверт
  на `SELFVOY_PUSH_WEBHOOK_URL`; подписывает JWT и форвардит в Apple шлюз) —
  в песочнице проверен путь до шлюза (webhook-драйвер, e2e), реальная
  доставка на устройство — на Mac с сертификатом/ключом `.p8`.
- Universal Links, Live Activities, виджет и Share Extension — позже.
  EventKit-календарь (события, не напоминания) — по потребности.
