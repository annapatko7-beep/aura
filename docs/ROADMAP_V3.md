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

## Уведомления (этап 13) — ГОТОВО

In-app «входящая» (таблица `notifications`, событие `notification.new`) +
push через внешний шлюз (`AURA_PUSH_DRIVER=webhook`; APNs-конверт с
`aps.alert/badge/sound`, JWT подписывает шлюз — C++-ядро без TLS/HTTP2).
Виды v1: `task.due` (напоминание), `confirmation.requested` (нужно
подтверждение), `a2a.proposal` (предложение другого агента), `login.new`
(новый вход), `twofactor.enabled`/`twofactor.disabled` (2FA вкл/выкл).
Настройки: отключённые виды и тихие часы — `prefs.notifications`
(`muted_kinds`, `quiet_hours` UTC; гасят только push); per-device —
вкл/выкл устройства в `push_devices`; звуки/важность — на стороне клиента.
Хуки: планировщик (task.due), барьер подтверждения, A2A-переговоры, вход
(обычный и 2FA), включение/отключение 2FA. UI: iOS-вкладка «Уведомления»
(+APNs-токен из AppDelegate, aps-environment), Qt — колокольчик с бейджем
(рельс DesktopShell и шапка ChatsPage) + панель с отзывом устройств.
«Подозрительная активность» отложена: критерии (геолокация, частота
попыток) появится вместе с этапом 14 (тестирование и безопасность).

## Тестирование и безопасность (этап 14) — ГОТОВО

Итоговое покрытие: C++ unit 55 тестов / 491 проверка, Python AI 83 теста,
e2e 137 проверок на реальном PostgreSQL (регистрация/2FA/OAuth+PKCE/ротация
refresh/разрешения/переговоры A2A/интеграции/уведомления/push).
Секьюрити-сценарии (5 новых C++-тестов + e2e-секция 5b): SQLi (литеральное
хранение, параметризованные запросы против живой PG), XSS (round-trip как
текст), CSRF (неприменим — токен в WS-кадре, не cookie), brute-force
(throttling по email+адресу, `forbidden` после порога), утечка токенов
(sessions/integrations/devices без секретов), повторное использование
refresh (ротация), обход подтверждения (барьер), доступ к чужой памяти/
чату/задачам (изоляция по user_id), подделка JWT. WebAuthn — исследование
(SECURITY.md §7), реализация не начата. Всё — в docs/TESTING.md: запуск
каждого съюта, матрица безопасности, ручные мобильные чек-листы (микрофон,
push, deep links, Siri/Команды, поворот, офлайн) и регрессионная процедура
релиза.

## Документация (этап 15)

README, ARCHITECTURE, SETUP, API, SECURITY, PRIVACY, MOBILE_FEATURES,
A2A_PROTOCOL, DATABASE, DESIGN_SYSTEM, TROUBLESHOOTING — с инструкциями
(.env, PostgreSQL, AI Service, сборка desktop/iOS, OAuth, push, 2FA,
Siri Shortcuts).

## Desktop и адаптивный UI (этап 12) — ГОТОВО

Реализовано: две раскладки Qt-клиента с мгновенным переключением по ширине
окна (Loader в `Main.qml`). Узкое окно (< 1120px) — прежняя стековая
навигация; широкое — `DesktopShell`: навигационный рельс (Чаты / Задачи /
Ждут / Ещё, бейдж неподтверждённых операций) + master-detail для чатов
(список 340px слева, диалог справа, подсветка выбранного). Список чатов
вынесен в общий `components/ChatList.qml`; страницы получили режим
`embedded` (без кнопок «назад»/«настройки»); геометрия окна запоминается
между запусками (`QtCore.Settings`). Попутно исправлена старая ошибка:
открытие существующего чата не грузило историю (никто не вызывал
`App.selectChat`). Проверка — qmlcachegen 17/17. Подробности —
docs/DESIGN.md «Адаптивность».

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
