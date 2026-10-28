# Тестирование Selfvoy (этап 14)

Три автоматических слоя + ручные чек-листы для мобильных платформ.

## Автоматические сьюты

### 1. C++ unit (ядро сервера) — 56 тестов, 497 проверок

```bash
cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSELFVOY_WITH_CLIENT=OFF -DSELFVOY_BUILD_TESTS=ON
cmake --build build/server -j4
cd build/server/server && ctest          # или ./selfvoy-tests [фильтр]
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

# 2) AI-сервис (общий с сервером токен; без него AI отвечает 503):
export SELFVOY_AI_TOKEN="$(openssl rand -hex 32)"
cd ai && PYTHONPATH=. uvicorn selfvoy_ai.app:app --host 127.0.0.1 --port 8000 &

# 3) C++-сервер (webhook-драйвер push + mock-адреса Google):
SELFVOY_PORT=9000 SELFVOY_DATABASE_URL="$DATABASE_URL" SELFVOY_AI_URL=http://127.0.0.1:8000 SELFVOY_AI_TOKEN="$SELFVOY_AI_TOKEN" \
SELFVOY_2FA_KEY=... SELFVOY_JWT_SECRET=... SELFVOY_MAIL_DRIVER=dev \
SELFVOY_PUSH_DRIVER=webhook SELFVOY_PUSH_WEBHOOK_URL=http://127.0.0.1:9082/push \
SELFVOY_GOOGLE_*=http://127.0.0.1:9081/... build/server/server/selfvoy-server &

# 4) прогон (mock-серверы Google и push поднимаются сами):
SELFVOY_E2E_GOOGLE_MOCK=1 SELFVOY_E2E_PUSH_MOCK=1 python3 tools/e2e.py

# сценарий админ-панели (§5a) — только если сервер запущен с SELFVOY_ADMIN_EMAILS,
# содержащим этот адрес; без переменных секция печатает «пропуск»:
SELFVOY_E2E_ADMIN_EMAIL=root@example.com SELFVOY_E2E_ADMIN_PASSWORD=... python3 tools/e2e.py

# Telegram-бот (§3b): мок Bot API поднимает сам e2e (порт SELFVOY_E2E_TELEGRAM_MOCK_PORT,
# по умолчанию 9083); сервер — с SELFVOY_TELEGRAM_BOT_TOKEN=e2e:token,
# SELFVOY_TELEGRAM_BOT_USERNAME=selfvoy_e2e_bot, SELFVOY_TELEGRAM_API_URL=http://127.0.0.1:9083:
SELFVOY_E2E_TELEGRAM_MOCK=1 python3 tools/e2e.py

# Оплата записи и ЮKassa (§5a+): мок API ЮKassa поднимает сам e2e (порт
# SELFVOY_E2E_YOOKASSA_MOCK_PORT, по умолчанию 9084; служебный /__succeed/<id> — «гость
# оплатил»); сервер — с SELFVOY_YOOKASSA_API_URL=http://127.0.0.1:9084. Гость этого
# сценария подключается с адреса 127.0.0.2 (свои лимиты гостевых методов по IP).
SELFVOY_E2E_YOOKASSA_MOCK=1 python3 tools/e2e.py
```

Покрытие: регистрация → email-код → вход → JWT → сессии; чат между
пользователями; агент (mock-LLM) с планом и действиями; разрешения и
барьер подтверждения; задачи, напоминания, планировщик; уведомления и
push-конверт в webhook-шлюз; STT через AI-сервис; 2FA (TOTP RFC 6238,
резервные коды, доверенные устройства); Google OAuth 2.0 + PKCE (календарь,
Gmail, авто-refresh, отзыв); Telegram-бот (привязка владельца и гостя по
подписанным ссылкам, отказ поддельной/просроченной, уведомление о записи, извещение
гостю, «Приду» из чужого чата, `/stop`, отвязка при 403, `telegram.unlink`); цена,
предоплата и политика отмены (ЮKassa: подключение, платёж, оплата, возврат; оплата по
ссылке: «Я оплатил(а)», отметка владельцем; `late_cancel=block`); тариф
(`admin.setPlan`, `user.plan`); сценарии безопасности; server.info.

### 4. Статические проверки без платформенных инструментов

```bash
python3 tools/check_ios_protocol.py       # Swift-типы сообщений ↔ хендлеры сервера
python3 tools/check_android_protocol.py   # Kotlin-типы сообщений ↔ хендлеры сервера
qmlcachegen ...                           # синтаксис QML (рецепт в Makefile/CI)
python3 -m py_compile tools/e2e.py        # синтаксис e2e
```

### 5. Android: JVM-тесты SelfvoyKit (без эмулятора)

Общий слой `:selfvoykit` — чистый Kotlin/JVM (протокол, deep links), поэтому
его тесты работают где угодно, где есть JDK 17:

```bash
cd android && ./gradlew :selfvoykit:test
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
| Брутфорс пароля | C++ `security_brute_force_throttle`; e2e §5b | после `SELFVOY_MAX_LOGIN_ATTEMPTS` (8) провалов вход/регистрация → `forbidden` до конца окна (`SELFVOY_LOGIN_WINDOW`, 300 с) |
| Утечка токенов | C++ `security_sessions_list_hides_secrets`, `integrations_list_hides_secret`, `server_push_devices`; e2e §5b | sessions.list/integrations.list/devices.push.list без токенов и хэшей |
| Повторное использование refresh | C++ `server_refresh_rotation_and_reuse` | ротация при каждом обмене, старый refresh отклоняется |
| Обход подтверждения | C++ `ai_permissions_ws_and_confirmation_barrier` | опасный инструмент без `confirmation.approve` не исполняется |
| Админ-панель: права и блокировка | C++ `admin_*` (7 тестов); e2e §5a | модератор не меняет роли, не трогает админов, не читает журнал; блокировка выбивает соединения, закрывает вход и старые токены |
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

### Deep links (`selfvoy://`)
- [ ] `selfvoy://chats/<id>` открывает чат;
- [ ] `selfvoy://voice` открывает голосовой оверлей;
- [ ] `selfvoy://ask?text=...` отправляет запрос в Selfvoy;
- [ ] `selfvoy://oauth?...` завершает подключение Google;
- [ ] неизвестный путь — игнорируется без краша.

### Siri и Команды (iOS)
- [ ] «Спросить Selfvoy …» через Siri → ответ Selfvoy;
- [ ] «Создать задачу …» → задача появляется в списке;
- [ ] Команды без разблокировки требуют аутентификацию (donate-предложения видны в приложении Команды).

### Android: шорткаты и FCM
- [ ] долгое нажатие на иконке → шорткаты «Спросить Selfvoy», «Голосом», «Новая задача» работают;
- [ ] первый вход: запрос POST_NOTIFICATIONS (Android 13+);
- [ ] токен FCM зарегистрирован («Входящая» → Push-устройства, платформа `fcm`);
- [ ] напоминание приходит push-ом при закрытом приложении (канал «selfvoy»);
- [ ] тап по push открывает «входящую» (`selfvoy://notifications`);
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
6. на Mac: `swift test` (SelfvoyKit), сборка Xcode, прогон мобильных
   чек-листов на симуляторе и устройстве;
7. `./gradlew :selfvoykit:test` (JDK 17) и, при наличии Android SDK,
   `./gradlew :app:assembleDebug` + чек-листы «Android: шорткаты и FCM».
