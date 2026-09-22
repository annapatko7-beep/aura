# Тестирование Aura (этап 14)

Три автоматических слоя + ручные чек-листы для мобильных платформ.

## Автоматические сьюты

### 1. C++ unit (ядро сервера) — 56 тестов, 497 проверок

```bash
cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DAURA_WITH_CLIENT=OFF -DAURA_BUILD_TESTS=ON
cmake --build build/server -j4
cd build/server/server && ctest          # или ./aura-tests [фильтр]
```

Встроенное хранилище (без PostgreSQL), AI-сервис не нужен. Покрытие:
JSON, crypto (Argon2id/HMAC/AES-256-GCM/base32), TOTP (RFC 6238), JWT,
WebSocket (RFC 6455), протокол, регистрация/вход/сброс пароля/смена пароля,
ротация и повторное использование refresh, сессии, чаты, память и настройки,
инструменты и разрешения, барьер подтверждения, задачи и планировщик,
2FA (полный поток), интеграции (PKCE S256, state, TTL, сокрытие секретов),
уведомления и push-устройства, сценарии безопасности (см. матрицу).

### 2. Python (AI-сервис) — 83 теста

```bash
cd ai && PYTHONPATH=. python -m pytest tests/
```

Покрытие: API (healthz, agent/run, negotiate, memory, planner, tools,
speech), агент (mock-LLM), память (BM25-ранжирование), планировщик,
речь, инструменты.

### 3. E2E (полный стек) — 140 проверок

Нужны: PostgreSQL, AI-сервис, C++-сервер, Python 3.11+.

```bash
# 1) PostgreSQL (например, pgserver или системный) + схема:
psql "$DATABASE_URL" -f schema/schema.sql
psql "$DATABASE_URL" -f schema/migrations/2026_09_20_notifications.sql
psql "$DATABASE_URL" -f schema/migrations/2026_09_20_push_fcm.sql

# 2) AI-сервис:
cd ai && PYTHONPATH=. uvicorn aura_ai.app:app --host 127.0.0.1 --port 8000 &

# 3) C++-сервер (webhook-драйвер push + mock-адреса Google):
AURA_PORT=9000 AURA_DATABASE_URL="$DATABASE_URL" AURA_AI_URL=http://127.0.0.1:8000 \
AURA_2FA_KEY=... AURA_JWT_SECRET=... AURA_MAIL_DRIVER=dev \
AURA_PUSH_DRIVER=webhook AURA_PUSH_WEBHOOK_URL=http://127.0.0.1:9082/push \
AURA_GOOGLE_*=http://127.0.0.1:9081/... build/server/server/aura-server &

# 4) прогон (mock-серверы Google и push поднимаются сами):
AURA_E2E_GOOGLE_MOCK=1 AURA_E2E_PUSH_MOCK=1 python3 tools/e2e.py
```

Покрытие: регистрация → email-код → вход → JWT → сессии; чат между
пользователями; агент (mock-LLM) с планом и действиями; разрешения и
барьер подтверждения; задачи, напоминания, планировщик; уведомления и
push-конверт в webhook-шлюз; STT через AI-сервис; 2FA (TOTP RFC 6238,
резервные коды, доверенные устройства); Google OAuth 2.0 + PKCE (календарь,
Gmail, авто-refresh, отзыв); сценарии безопасности; server.info.

### 4. Статические проверки без платформенных инструментов

```bash
python3 tools/check_ios_protocol.py       # Swift-типы сообщений ↔ хендлеры сервера
python3 tools/check_android_protocol.py   # Kotlin-типы сообщений ↔ хендлеры сервера
qmlcachegen ...                           # синтаксис QML (рецепт в Makefile/CI)
python3 -m py_compile tools/e2e.py        # синтаксис e2e
```

### 5. Android: JVM-тесты AuraKit (без эмулятора)

Общий слой `:aurakit` — чистый Kotlin/JVM (протокол, deep links), поэтому
его тесты работают где угодно, где есть JDK 17:

```bash
cd android && ./gradlew :aurakit:test
```

CI делает то же самое (job `android`). Полная сборка APK требует Android
SDK — см. docs/ANDROID.md.

Swift-сборка и Qt-сборка требуют macOS/Xcode и Qt 6 — в песочнице
разработки их нет (см. ROADMAP «Ограничения песочницы»).

## Матрица сценариев безопасности

| сценарий | где покрыт | результат |
| --- | --- | --- |
| SQL-инъекции (память, задачи, поиск, login) | C++ `security_injection_payloads_are_literal`; e2e §5b против реального PostgreSQL | текст хранится буквально (параметризованные запросы), таблицы живы, поиск пуст |
| XSS в сообщениях | C++ `security_xss_message_roundtrip_is_literal` | `<script>` round-trip как текст; клиенты (QML Text, SwiftUI Text) HTML не рендерят |
| CSRF | протокол: авторизация токеном в WS-кадре, не cookie | неприменим; неаутентифицированные кадры отклоняются (`server_unknown_type_and_auth_guard`) |
| Брутфорс пароля | C++ `security_brute_force_throttle`; e2e §5b | после `AURA_MAX_LOGIN_ATTEMPTS` (8) провалов вход/регистрация → `forbidden` до конца окна (`AURA_LOGIN_WINDOW`, 300 с) |
| Утечка токенов | C++ `security_sessions_list_hides_secrets`, `integrations_list_hides_secret`, `server_push_devices`; e2e §5b | sessions.list/integrations.list/devices.push.list без токенов и хэшей |
| Повторное использование refresh | C++ `server_refresh_rotation_and_reuse` | ротация при каждом обмене, старый refresh отклоняется |
| Обход подтверждения | C++ `ai_permissions_ws_and_confirmation_barrier` | опасный инструмент без `confirmation.approve` не исполняется |
| Доступ к чужой памяти/чату/задачам | C++ `security_cross_user_isolation`; e2e §5b; `server_chat_flow_and_delivery` (forbidden) | выборки строго по user_id |
| Подделка JWT | C++ `jwt_sign_and_verify` | чужой секрет, истёкший, битый формат — отклоняются |
| Раскрытие существования аккаунта | код authmanager | «неверный email или пароль» одинаково для несуществующего email и неверного пароля |
| WebAuthn/passkeys | SECURITY.md §7 | исследование; реализация не начата — тестировать нечего (честно) |

## Мобильные чек-листы (ручные, на устройстве)

### Микрофон и голос
- [ ] первый запуск: запрос разрешения на микрофон и распознавание речи;
- [ ] отказ от микрофона → текстовый ввод работает, крашей нет;
- [ ] запись → волна/уровень реагируют; отмена стирает запись;
- [ ] офлайн: on-device распознавание (iOS SFSpeechRecognizer) или ошибка без краша;
- [ ] TTS озвучивает ответ; режим «только важное» уважается.

### Push-уведомления
- [ ] первый вход: системный запрос разрешения на уведомления;
- [ ] разрешение → токен APNs зарегистрирован (вкладка «Уведомления» → устройства);
- [ ] напоминание (task.due) приходит push-ом при закрытом приложении;
- [ ] тихие часы (prefs.notifications.quiet_hours) гасят push, in-app остаётся;
- [ ] muted_kinds отключает выбранные типы;
- [ ] отзыв устройства в списке прекращает push на него;
- [ ] тап по push открывает приложение (бейдж обновляется).

### Deep links (`aura://`)
- [ ] `aura://chats/<id>` открывает чат;
- [ ] `aura://voice` открывает голосовой оверлей;
- [ ] `aura://ask?text=...` отправляет запрос Ауре;
- [ ] `aura://oauth?...` завершает подключение Google;
- [ ] неизвестный путь — игнорируется без краша.

### Siri и Команды (iOS)
- [ ] «Спросить Ауру …» через Siri → ответ Ауры;
- [ ] «Создать задачу …» → задача появляется в списке;
- [ ] Команды без разблокировки требуют аутентификацию (donate-предложения видны в приложении Команды).

### Android: шорткаты и FCM
- [ ] долгое нажатие на иконке → шорткаты «Спросить Ауру», «Голосом», «Новая задача» работают;
- [ ] первый вход: запрос POST_NOTIFICATIONS (Android 13+);
- [ ] токен FCM зарегистрирован («Входящая» → Push-устройства, платформа `fcm`);
- [ ] напоминание приходит push-ом при закрытом приложении (канал «aura»);
- [ ] тап по push открывает «входящую» (`aura://notifications`);
- [ ] без `google-services.json` настройки честно показывают «Push не настроен», крашей нет.

### Поворот и адаптивность
- [ ] портрет ↔ ландшафт: состояние экрана не теряется;
- [ ] ширина ≥ 1120 (iPad/десктоп) → рельс + master-detail; < 1120 → стек;
- [ ] выбранный чат переживает смену раскладки;
- [ ] клавиатура не перекрывает поле ввода.

### Офлайн и сеть
- [ ] потеря сети → статус «нет соединения», история на месте;
- [ ] восстановление → авто-переподключение, токен из Keychain;
- [ ] истёкший access → прозрачный обмен refresh;
- [ ] смена адреса сервера в настройках → переподключение.

## Регрессионная процедура релиза

1. `ctest` (C++ unit) — 0 ошибок;
2. `pytest ai/tests` — 0 ошибок;
3. e2e на чистом кластере PostgreSQL (схема + миграции) — 0 ошибок;
4. `check_ios_protocol.py` и `check_android_protocol.py` — OK;
5. qmlcachegen по всем QML — 0 ошибок;
6. на Mac: `swift test` (AuraKit), сборка Xcode, прогон мобильных
   чек-листов на симуляторе и устройстве;
7. `./gradlew :aurakit:test` (JDK 17) и, при наличии Android SDK,
   `./gradlew :app:assembleDebug` + чек-листы «Android: шорткаты и FCM».
