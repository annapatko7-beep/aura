# Aura для Android (этап 11)

Нативный клиент: **Kotlin + Jetpack Compose**, минимальный API 26
(Android 8.0), целевой API 34. Общий слой протокола — модуль `:aurakit`
(чистый Kotlin/JVM, без Android-зависимостей): тот же WS-протокол, что у
desktop- и iOS-клиентов (docs/PROTOCOL.md).

## Структура

```
android/
├── settings.gradle.kts, build.gradle.kts, gradle/libs.versions.toml
├── aurakit/                          # общий слой (аналог AuraKit на iOS)
│   └── src/main/kotlin/ai/aura/kit/  # Protocol, WebSocketTransport,
│                                     # AuraClient, DeepLink
│   └── src/test/kotlin/…             # JVM-тесты (без эмулятора)
└── app/                              # Compose-приложение
    ├── google-services.json.example  # шаблон конфига Firebase
    └── src/main/
        ├── AndroidManifest.xml       # разрешения, deep links, FCM-сервис
        ├── kotlin/ai/aura/app/       # MainActivity, AppStore, SecureStore,
        │                             # Notifications, push/FcmService,
        │                             # voice/VoiceService, ui/*
        └── res/                      # тема, иконки, шорткаты лаунчера
```

Принцип v3 сохранён: клиенты раздельные, общие — только протокол и сервер.

## Сборка

Нужны: **Android Studio** (Koala или новее) либо JDK 17 + Android SDK 34.
Gradle-обёртка в репозитории не лежит (бинарь) — при первом открытии
Android Studio сгенерирует её сам, либо вручную:

```bash
cd android
gradle wrapper --gradle-version 8.7   # если gradle установлен
./gradlew :app:assembleDebug
```

Тесты общего слоя — без эмулятора и SDK (только JDK 17):

```bash
./gradlew :aurakit:test
```

### Адрес сервера

По умолчанию debug-сборка смотрит на `ws://10.0.2.2:9000` — это хост
разработчика глазами эмулятора. Переопределение:

- `android/local.properties`: `aura.server.url=ws://192.168.1.10:9000`
  (файл в git не попадает);
- или в самом приложении: экран входа → «Адрес сервера» (для прода —
  `wss://` за TLS-прокси, см. docs/SETUP.md).

### Firebase / FCM (push)

1. Создайте проект в консоли Firebase, добавьте приложение Android с
   пакетом `ai.aura.app`;
2. скачайте `google-services.json` и положите в `android/app/`
   (в git не попадает — `.gitignore`; шаблон: `google-services.json.example`);
3. при сборке без этого файла плагин google-services не применяется,
   FirebaseApp не инициализируется, а приложение честно показывает
   «Push не настроен» — остальные функции работают.

Доставка — через внешний шлюз (у C++-сервера нет TLS и ключей Google):
сервер кладёт в вебхук конверт `{platform:"fcm", token, kind,
fcm:{url, message}}` (docs/PROTOCOL.md), шлюз получает OAuth2-токен
сервисного аккаунта (role: Firebase Cloud Messaging) и делает
`POST {fcm.url}` с `Authorization: Bearer <токен>` и телом `message`.
`AURA_FCM_URL` должен содержать id вашего проекта Firebase.

## Разрешения — только необходимые

| разрешение | назначение | когда запрашивается |
| --- | --- | --- |
| `INTERNET` | WS-соединение | install-time |
| `POST_NOTIFICATIONS` | уведомления/push (API 33+) | при первом входе |
| `RECORD_AUDIO` | голосовой ввод | при нажатии «Слушать» |

Accessibility API не используется; фоновых процессов нет (FCM-сервис —
официальный механизм доставки).

## Возможности

- **Deep links** `aura://chats/<id> | voice | ask?text=… | tasks |
  notifications | settings | oauth` — те же маршруты, что на iOS.
- **Шорткаты лаунчера** (долгое нажатие на иконке): «Спросить Ауру»,
  «Голосом», «Новая задача»; activity принимает `ACTION_ASSIST`.
- **Голос**: системный SpeechRecognizer (on-device), fallback — серверный
  STT (`speech.transcribe`, WAV 16 кГц моно); озвучка — TextToSpeech.
- **Google OAuth**: Chrome Custom Tabs → редирект `aura://oauth` →
  `integrations.callback`. Токены провайдеров клиент не видит.
- **2FA**: TOTP + резервные коды; доверенное устройство — по стабильному
  `device_id`.
- **Секреты**: токены и адрес сервера — EncryptedSharedPreferences
  (Android Keystore).

## Проверки (что прогнано и что нет)

- `tools/check_android_protocol.py` — все строковые типы сообщений
  Kotlin-клиента известны серверу (CI job `protocol`);
- JVM-тесты `:aurakit` (кадры протокола, deep links) — CI job `android`;
- серверная часть FCM покрыта C++ unit-тестом `server_push_devices_fcm`
  и e2e-секцией 3e (конверт `{platform:"fcm", fcm.message…}` ловится
  push-моком);
- **сборка APK в песочнице разработки не выполнялась** (нет Android SDK):
  код написан под compileSdk 34 / AGP 8.5 / Kotlin 2.0 и проверялся
  статически; перед релизом соберите `:app:assembleDebug` в Android Studio
  и прогоните ручные чек-листы (docs/TESTING.md, «Android: шорткаты и FCM»).
