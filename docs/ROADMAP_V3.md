# Aura v3 — целевая архитектура и план (этап 3)

Статус: **предложение на утверждение**. Реализация начинается после
подтверждения дизайна и архитектуры (этап 5+).

## Зафиксированные решения (ответы владельца)

| Вопрос | Решение |
| --- | --- |
| Платформы v1 | **Desktop (Win/macOS/Linux) + iOS**; Android и Web — позже |
| Код | **Раздельные клиенты**: Qt 6/QML для desktop, **Swift/SwiftUI** для iOS; общий только протокол и backend |
| Голос | **Гибрид**: on-device когда доступно (iOS Speech Framework), сервер (Whisper-совместимый STT) — fallback и диктовка; TTS платформенный, ru+en+авто |
| Вход | Email+пароль, **Google OAuth, Apple OAuth, passkey (WebAuthn), TOTP 2FA + recovery-коды**; без SMS |
| Интеграции v1 | **Реальный Google OAuth** (Calendar + Gmail), токены зашифрованы |
| Дизайн | выбирается на этапе 4 из трёх вариантов |

## Целевая архитектура

```
┌─────────────┐  ┌─────────────┐
│ Qt/QML app  │  │ iOS app     │        WS {id,type,payload} / REST
│ desktop     │  │ SwiftUI     │──────┐
└─────────────┘  └─────────────┘      │
  App Intents / Siri / Share ext ─────┤
                                      ▼
                      ┌───────────────────────────┐
                      │ C++ Server (Listener)     │
                      │ Session → ConnectionManager│
                      │ AuthManager (Argon2id,     │
                      │   OAuth, WebAuthn, TOTP)   │
                      │ SessionManager, RateLimiter│
                      │ ChatManager, AgentManager  │
                      │ NegotiationEngine          │
                      │ MemoryManager, ToolManager │
                      │ PermissionGuard, Scheduler │
                      │ NotificationManager        │
                      │ Repositories, AuditLog     │
                      └──────┬───────────┬────────┘
                             │           │ HTTP (внутр.)
                             ▼           ▼
                  ┌──────────────┐  ┌──────────────┐
                  │ PostgreSQL   │  │ Python AI    │
                  │ + pgvector   │  │ FastAPI      │
                  │ (22 таблицы) │  │ agent/plan/  │
                  └──────────────┘  │ memory/RAG/  │
                  ┌──────────────┐  │ negotiator/  │
                  │ Redis (опц.: │  │ tools)       │
                  │ rate-limit,  │  └──────┬───────┘
                  │ кэш сессий)  │         ▼
                  └──────────────┘  LLM-провайдер (или mock)
```

Принцип безопасности: **LLM только формирует намерение**. Любое действие
исполняет C++-сервер после проверки `tool_permissions` и (для опасных
операций) подтверждения пользователя. Подтверждение — синхронный барьер:
задача ждёт `confirmation.approve`.

## Слой auth и безопасности (этапы 5–6)

- Пароль: **Argon2id** (миграция с PBKDF2: при успешном логине старый хеш
  пересчитывается в новый формат), проверка формата email на клиенте и сервере.
- Токены: access 15 мин + refresh с **ротацией** и детектом повторного
  использования (reuse → отзыв всей цепочки).
- Подтверждение email (одноразовый код), восстановление и смена пароля.
- OAuth: Google и Apple (PKCE), связывание с существующим аккаунтом по
  верифицированному email; refresh-токены провайдеров шифруются (AES-256-GCM,
  ключ из env).
- WebAuthn/passkey: регистрация и аутентификация, хранение креденшелов,
  порядок входа «пароль → TOTP/passkey → сессия».
- TOTP: QR + ручной секрет (RFC 6238), recovery-коды (хеш), включение/отключение
  только после повторной проверки, доверенные устройства, журнал событий.
- Rate limit + brute-force защита: уже есть (attempts/lockout в AuthManager),
  расширяем до отдельного RateLimiter (по IP, email, endpoint) с опциональным
  Redis для нескольких инстансов.
- Ошибки авторизации — без раскрытия существования email.

## Голос (этап 7)

- Состояния: idle → listening → processing → success/error; волна, отмена
  свайпом, редактирование расшифровки перед отправкой, авто-отправка (настройка).
- Гибрид: iOS — Speech Framework on-device; desktop — серверный STT;
  сервер — endpoint `/v1/speech/transcribe` (Whisper-совместимый), язык
  ru/en/auto.
- TTS: платформенный (AVSpeechSynthesizer / Qt TextToSpeech), настройки
  голоса/скорости/громкости, режим «только важные».
- Отказ микрофона → текстовый fallback; понятные сообщения об ошибках.

**Статус этапа 7 (десктоп-часть): реализовано.** Серверный STT-эндпоинт
`POST /v1/speech/transcribe` (Python AI Service, провайдер `mock` без ключей и
Whisper-бэкенд при `AURA_STT_URL`/`AURA_STT_API_KEY`), WS-метод
`speech.transcribe` в C++-сервере (форвард аудио), захват микрофона
(Qt Multimedia, 16 кГц/моно/16 бит → WAV → base64) и платформенный TTS
(Qt TextToSpeech) в `AppStore`, QML-панель `VoicePanel` (состояния, волна,
отмена, правка расшифровки, автоотправка) и настройки голоса/озвучки.
iOS-часть (Speech Framework on-device, AVSpeechSynthesizer) — в этапе 10.

## iOS-приложение (этап 10) — ГОТОВО — нативный Swift

Реализовано: SwiftUI-приложение (`ios/Aura`) + общий Swift-пакет **AuraKit**
(WS-транспорт, протокол, AuraClient, Keychain, deep links); вход/2FA, чаты,
«Спросить Ауру», задачи (+ экспорт в напоминания iOS через EventKit по
кнопке), подтверждения, разрешения, интеграции Google (Safari → `aura://oauth`),
гибридный голос (SFSpeechRecognizer + серверный STT-fallback, озвучка
AVSpeechSynthesizer). App Intents «Спросить Ауру»/«Создать задачу» + фразы
Siri; deep links `aura://voice|chats/{id}|ask|tasks|settings|oauth`.
**Back Tap напрямую недоступен сторонним приложениям** — в Quick Actions:
пошаговая инструкция (Универсальный доступ → Касание → Касание сзади →
Команды → «Спросить Ауру»). Тесты: AuraKitTests (`swift test`), AuraTests;
сверка протокола — `tools/check_ios_protocol.py`. Подробности — docs/IOS.md.

Осталось в этапе 13: Push (APNs, Notification Service Extension), Live
Activities для активных переговоров, виджет «быстрый запрос Ауре», Share
Extension.

## Интеграции (этап 9) — ГОТОВО

Реализовано: Google Calendar + Gmail через реальный OAuth 2.0 Authorization
Code + PKCE (S256); `IntegrationManager` (begin/callback/list/revoke/sync),
WS-хендлеры `integrations.*`, таблицы `integration_connections` и
`integration_oauth_states`, токены — только зашифрованными (AES-256-GCM),
авто-refresh по `refresh_token`, отзыв доступа у провайдера и локально.
Инструменты `send_email`/`check_calendar` используют подключения прозрачно.
UI — секция «Интеграции» в настройках клиента (системный браузер +
loopback-редирект). Подробности — PROTOCOL.md «Интеграции», SECURITY.md §9.

Исходный план: общий интерфейс `IntegrationProvider` (OAuth
connect/disconnect/re-auth, журнал использования, ошибки). В v1: **Google
Calendar + Gmail** (реальный OAuth), остальные — песочница с тем же
контрактом. Токены — только зашифрованными, доступ к календарю — всегда
через PermissionGuard.

## База данных (этап 13 документации + миграции)

Есть 7 таблиц: `users sessions chats chat_members messages user_memory
user_preferences` (+ вьюхи `chat_previews`, `active_sessions`).
Добавить (миграциями): `user_profiles agents agent_preferences negotiations
negotiation_messages tasks tool_permissions integrations oauth_tokens
refresh_tokens two_factor_settings recovery_codes trusted_devices audit_logs
notifications`. Плюс: FK с каскадами, индексы, timestamps, soft delete
(`deleted_at`) для сообщений/памяти, шифрование чувствительных полей на
уровне приложения, RAG через pgvector с фолбэком на ключевой поиск, когда
расширение недоступно.

## Уведомления (этап 13)

In-app + push (APNs для iOS). Типы: задача завершена, нужно подтверждение,
предложение другого агента, напоминание, новый вход, 2FA вкл/выкл, подозрительная
активность. Настройки: типы, тихие часы, звуки, важность, per-device.

## Тестирование и безопасность (этап 14)

Существующее ядро: C++ unit 160 проверок, Python 58 тестов, e2e 23 сценария.
Добавить: тесты регистрации/2FA/OAuth/WebAuthn/ротации refresh/разрешений/
переговоров; мобильные чек-листы (микрофон, push, deep links, Shortcuts,
поворот, offline); секьюрити-сценарии: SQLi, XSS, CSRF, brute-force, утечка
токенов, повторное использование refresh, обход подтверждения, доступ к чужой
памяти.

## Документация (этап 15)

README, ARCHITECTURE, SETUP, API, SECURITY, PRIVACY, MOBILE_FEATURES,
A2A_PROTOCOL, DATABASE, DESIGN_SYSTEM, TROUBLESHOOTING — с инструкциями
(.env, PostgreSQL, AI Service, сборка desktop/iOS, OAuth, push, 2FA,
Siri Shortcuts).

## Ограничения песочницы (честно)

- **Собрать iOS-приложение здесь нельзя** (нет macOS/Xcode): поставляем
  Xcode-проект + SPM-пакет, которые собираются на вашем Mac; проверим код
  статически и тестами логики, но не компилятором.
- Реальные ключи Google OAuth / APNs выдаются только вам: реализуем потоки
  и тестируем против локального mock-провайдера, ключи — через `.env`.
- pgvector/Redis в песочнице могут быть недоступны: код работает с ними
  при наличии и деградирует gracefully.

## Порядок этапов (с подтверждением после каждого)

5 — Auth/регистрация/сессии · 6 — 2FA и безопасность · 7 — голос и озвучка ·
8 — AI-функции · 9 — интеграции и разрешения · 10 — iOS и Shortcuts ·
11 — Android (отложен, вне v1) · 12 — desktop и адаптивный UI · 13 — push ·
14 — тестирование · 15 — документация · 16 — production-сборка.
