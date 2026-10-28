# Мобильные возможности Selfvoy

## iOS (готово, этап 10 + 13)

Нативное приложение: Swift/SwiftUI, общий слой `SelfvoyKit` (SPM) — тот же
WS-протокол, что у desktop-клиента. Сборка — [IOS.md](IOS.md).

### Siri и Команды (App Intents)

- **«Спросить Selfvoy …»** (`AskSelfvoyIntent`) — запрос к агенту без открытия
  приложения; параметр `text` подставляется из фразы. Ответ озвучивается
  и возвращается в диалог Siri.
- **«Создать задачу …»** (`CreateTaskIntent`) — быстрая задача
  (`tasks.create`), опционально с напоминанием.
- Команды появляются в приложении «Команды» (donate при первом запуске);
  работают из Siri, с экрана блокировки и из Back Tap (через привязку
  Команды — см. ниже).
- Сессия для интентов: `IntentSession` поднимает собственное WS-соединение
  с токеном из Keychain — без UI и без фоновых процессов.

### Back Tap — честно

Back Tap — системная функция iOS (Настройки → Универсальный доступ →
Касание → Касание задней панелью). Стороннее приложение **не может**
перехватывать постукивания напрямую — любой «свой Back Tap» в App Store
был бы обманом. Selfvoy делает по-честному:

1. регистрирует Команды («Спросить Selfvoy», «Создать задачу») в системе;
2. показывает в приложении пошаговую инструкцию: привяжите Команду к
   двойному/тройному постукиванию в системных настройках;
3. дальше Back Tap работает системно: постучали → Команда → Selfvoy.

Никаких Accessibility API, никаких фоновых слушателей, никаких лишних
разрешений.

### Deep links (`selfvoy://`)

| ссылка | действие |
| --- | --- |
| `selfvoy://chats/<id>` | открыть чат |
| `selfvoy://voice` | голосовой оверлей |
| `selfvoy://ask?text=…` | отправить запрос в Selfvoy |
| `selfvoy://tasks` | вкладка задач |
| `selfvoy://settings` | настройки |
| `selfvoy://oauth?provider&code&state` | возврат Google OAuth (redirect URI интеграций) |
| `selfvoy://signup?ref=CODE` | форма регистрации с кодом приглашения (Android, iOS; без входа, после входа игнорируется) |

Неизвестные пути игнорируются без краша (покрыто чек-листом TESTING.md).

### Push-уведомления (этап 13)

- APNs-токен регистрируется автоматически после разрешения
  (`devices.push.register`, platform `apns`); доставка — через внешний
  шлюз сервера (SECURITY.md §10).
- Вкладка «Уведомления»: входящая (badge непрочитанных), «Прочитать все»,
  список push-устройств с отзывом.
- Бейдж иконки = число непрочитанных; тихие часы и `muted_kinds` — в
  настройках (`prefs.notifications`).

### Прочее

- **Голос**: on-device SFSpeechRecognizer + fallback на серверный STT;
  озвучка AVSpeechSynthesizer; режим «только важное».
- **Напоминания iOS**: экспорт задач в EventKit — только по явной кнопке
  (`requestFullAccessToReminders` в момент действия).
- **Keychain**: токены и адрес сервера — только
  `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`.

## Android (готово, этап 11)

Нативное приложение: Kotlin + Jetpack Compose, общий слой `:selfvoykit`
(чистый Kotlin/JVM — тот же WS-протокол, что у desktop и iOS; его тесты
гоняются без эмулятора). Сборка — [ANDROID.md](ANDROID.md).

### Push: Firebase Cloud Messaging

- Токен FCM регистрируется после входа (`devices.push.register`, платформа
  `fcm`); ротация токена (`onNewToken`) — повторный upsert.
- Сервер собирает для FCM-устройств конверт HTTP v1 API
  (`fcm: {url, message}`) и отдаёт его внешнему шлюзу
  (`SELFVOY_PUSH_DRIVER=webhook`): шлюз подписывает OAuth2-токен сервисного
  аккаунта Google и форвардит `message`. Ключи Google на C++-сервере не
  хранятся — так же, как ключ APNs.
- Системная доставка идёт в канал `selfvoy` (создаётся приложением); в
  foreground карточку показывает само приложение и обновляет «входящую».
- Без `app/google-services.json` (в репозиторий не попадает) push честно
  показывает «не настроен» — остальные функции работают.

### Шорткаты и голос — только официальные механизмы

- **Шорткаты лаунчера** (`res/xml/shortcuts.xml`, долгое нажатие на иконке):
  «Спросить Selfvoy», «Голосом», «Новая задача» — каждый открывает приложение
  по deep link `selfvoy://…`.
- **Ассистент**: activity принимает `android.intent.action.ASSIST`.
- **Голос**: системный `SpeechRecognizer` (on-device), fallback — запись
  16 кГц моно → WAV → `speech.transcribe` (серверный Whisper-совместимый
  STT); озвучка — платформенный `TextToSpeech`.
- **Никаких Accessibility API, фоновых слушателей и «своего Back Tap»** —
  на Android аналога Back Tap нет, и имитировать его мы не будем.

### Разрешения (минимум)

| разрешение | зачем | когда запрашивается |
| --- | --- | --- |
| `INTERNET` | WS-соединение | install-time |
| `POST_NOTIFICATIONS` | уведомления и push (Android 13+) | при первом входе |
| `RECORD_AUDIO` | голосовой ввод | при нажатии «Слушать» |

### Deep links (`selfvoy://`)

Тот же набор, что на iOS (таблица выше), плюс `selfvoy://notifications` —
тап по push-уведомлению открывает «входящую». Неизвестные пути
игнорируются без краша.

### Google OAuth

`integrations.begin` → Chrome Custom Tabs → редирект `selfvoy://oauth`
возвращает пользователя в приложение (singleTask + intent-filter), дальше —
`integrations.callback`. Токены провайдеров клиенту не отдаются.

### Хранение секретов

Access/refresh-токены и адрес сервера — `EncryptedSharedPreferences`
(ключи в Android Keystore, аналог Keychain на iOS). `device_id` — обычный
SharedPreferences: это не секрет, а идентификатор для доверенных устройств.

## Чек-листы ручной проверки

Микрофон, push, deep links, Siri/Команды, поворот, офлайн —
[TESTING.md](TESTING.md), раздел «Мобильные чек-листы».
