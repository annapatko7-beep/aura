# Установка и настройка Aura

Порядок: PostgreSQL → AI Service → C++-сервер → клиенты. Для разработки
достаточно `make`-целей; для продакшена — те же шаги плюс реальные секреты
и TLS-терминирующий прокси.

## 1. Требования

- Linux/macOS, C++17-компилятор (GCC 12+ / Clang), CMake ≥ 3.21, Ninja;
- Python 3.11+;
- PostgreSQL 14+ (разработка: подойдёт `pgserver` из pip);
- Qt 6.5+ для desktop-клиента (Core, Gui, Network, Qml, Quick,
  QuickControls2, WebSockets, Multimedia, TextToSpeech);
- macOS + Xcode 15 + XcodeGen для iOS-клиента.

## 2. PostgreSQL

```bash
createdb aura
psql aura -v ON_ERROR_STOP=1 -f schema/schema.sql
# обновления схемы — идемпотентные файлы из schema/migrations/:
psql aura -v ON_ERROR_STOP=1 -f schema/migrations/2026_09_20_notifications.sql
# демо-данные (необязательно):
psql aura -f schema/seed.sql
```

Без `AURA_DATABASE_URL` сервер использует встроенное JSON-хранилище
(`AURA_EMBEDDED_DB`) — только для разработки и тестов.

## 3. Python AI Service

```bash
make venv                     # .venv + ai/requirements.txt
cd ai && PYTHONPATH=. uvicorn aura_ai.app:app --host 127.0.0.1 --port 8000
```

Ключевые переменные (полный список — `ai/aura_ai/config.py`):

| переменная | смысл |
| --- | --- |
| `AURA_AI_TOKEN` | общий секрет C++ ↔ AI (заголовок `X-Aura-Token`); без него эндпоинты открыты — только для localhost |
| `AURA_LLM_PROVIDER` | `mock` (по умолчанию) или `openai` |
| `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `AURA_LLM_MODEL` | реальный LLM |
| `AURA_MEMORY_BACKEND` | `file` (по умолчанию) или `postgres` |
| `AURA_RAG_MODE` | `auto`: BM25 всегда, эмбеддинги если заданы |
| `AURA_EMBEDDINGS_URL/_MODEL/_API_KEY` | семантический поиск памяти |
| `AURA_STT_PROVIDER/_URL/_MODEL/_API_KEY` | Whisper-совместимый STT; без них — mock |
| `AURA_CORS_ORIGINS` | для браузерных клиентов |

## 4. C++-сервер

```bash
cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DAURA_WITH_LIBPQ=ON -DAURA_BUILD_TESTS=ON -DAURA_WITH_CLIENT=OFF
cmake --build build/server -j
AURA_DATABASE_URL="postgresql://user:pass@host/aura" \
AURA_AI_URL=http://127.0.0.1:8000 \
AURA_JWT_SECRET="$(openssl rand -hex 32)" \
AURA_2FA_KEY="$(openssl rand -hex 32)" \
./build/server/server/aura-server
```

Переменные сервера (значения по умолчанию — в `server/include/aura/config.h`):

| группа | переменные |
| --- | --- |
| сеть | `AURA_HOST` (0.0.0.0), `AURA_PORT` (9000), `AURA_MAX_CONNECTIONS`, `AURA_MAX_FRAME`, `AURA_PING_INTERVAL`, `AURA_SOCKET_TIMEOUT` |
| БД | `AURA_DATABASE_URL`, `AURA_EMBEDDED_DB` |
| AI | `AURA_AI_URL`, `AURA_AI_TOKEN`, `AURA_AI_TIMEOUT_MS` |
| секреты | `AURA_JWT_SECRET` (обязателен в проде), `AURA_2FA_KEY` (шифр TOTP-секретов и токенов интеграций) |
| токены/лимиты | `AURA_TOKEN_TTL` (7 дней), `AURA_REFRESH_TTL` (30 дней), `AURA_VERIFY_TTL`, `AURA_RESET_TTL`, `AURA_MAX_LOGIN_ATTEMPTS` (8), `AURA_LOGIN_WINDOW` (300 с) |
| почта | `AURA_MAIL_DRIVER` (`dev` — коды в ответе и логе; `http` + `AURA_EMAIL_API_URL`) |
| Google OAuth | `AURA_GOOGLE_CLIENT_ID/_SECRET`, `AURA_GOOGLE_AUTH_URL/_TOKEN_URL/_REVOKE_URL/_CALENDAR_URL/_GMAIL_URL`, `AURA_OAUTH_STATE_TTL` |
| push | `AURA_PUSH_DRIVER` (`dev`/`webhook`), `AURA_PUSH_WEBHOOK_URL`, `AURA_APNS_TOPIC` (ai.aura.app), `AURA_APNS_URL` |
| прочее | `AURA_SCHEDULER_INTERVAL_MS` (0 — только ручной tick), `AURA_TOOLS_MODE`, `AURA_MAPS_API_URL`, `AURA_LOG_LEVEL` |

### Google OAuth (интеграции Calendar/Gmail)

1. Google Cloud Console → OAuth Client (Web application).
2. Redirect URI: desktop — `http://127.0.0.1:<порт>/callback` (клиент
   поднимает loopback-сервер сам); iOS — `aura://oauth`.
3. Scopes: `calendar.events.readonly`, `gmail.send`.
4. Заполните `AURA_GOOGLE_*`; без client_id интеграции отвечают
   `not_configured` (честно, без заглушек).

### Push (APNs через шлюз)

Ядро не делает TLS/HTTP2/ES256 — это задача внешнего шлюза:

```
AURA_PUSH_DRIVER=webhook
AURA_PUSH_WEBHOOK_URL=https://push-gateway.internal/push
```

Сервер POST-ит шлюзу конверт `{platform, token, notification_id, kind,
apns: {url, headers, payload}}`; шлюз подписывает JWT ключом `.p8` и
форвардит `apns.payload` на `apns.url`. Драйвер `dev` логирует доставки.

## 5. Desktop-клиент (Qt)

```bash
cmake -S . -B build/client -G Ninja -DAURA_WITH_CLIENT=ON
cmake --build build/client -j
./build/client/client/aura-client
```

Адрес сервера задаётся в окне входа (сохраняется в QSettings вместе с
токенами). Для продакшена — `wss://` за TLS-прокси.

## 6. iOS-клиент

```bash
brew install xcodegen
cd ios && xcodegen generate && open Aura.xcodeproj
```

- В схеме Aura включите Push Notifications (entitlement `aps-environment`
  уже в `project.yml`) и свой bundle id `ai.aura.app`.
- Адрес сервера — в поле входа (`wss://…` для ATS; `ws://` только для
  локальной разработки с исключением в Info.plist).
- Siri/Команды: App Intents регистрируются при первом запуске; Back Tap
  настраивается пользователем в системных настройках через Команду —
  приложение честно показывает пошаговую инструкцию (см. MOBILE_FEATURES).

## 7. 2FA (TOTP)

Работает из коробки: `auth.setup2fa` → QR/otpauth URI → `auth.confirm2fa`
→ 10 резервных кодов. Секреты шифруются ключом `AURA_2FA_KEY` (fallback —
`AURA_JWT_SECRET`); в продакшене задайте отдельный `AURA_2FA_KEY` и не
меняйте его (иначе существующие секреты не расшифруются).

## 8. Production-стек через Docker Compose

Весь серверный стек (PostgreSQL 16 + AI Service + C++-сервер) поднимается
одной командой; Qt-клиент собирается на хосте (нужен GUI), iOS — на Mac.

```bash
# .env-файл рядом с docker-compose.yml (в git не коммитить):
#   AURA_JWT_SECRET=…        AURA_2FA_KEY=…        POSTGRES_PASSWORD=…
#   AURA_AI_TOKEN=…          OPENAI_API_KEY=…      AURA_LLM_PROVIDER=openai
#   AURA_PUSH_DRIVER=webhook AURA_PUSH_WEBHOOK_URL=https://gateway/push
#   AURA_GOOGLE_CLIENT_ID=…  AURA_GOOGLE_CLIENT_SECRET=…

make docker-build   # собрать образы (server: multi-stage CMake+libpq; ai: python:3.12-slim)
make docker-up      # поднять стек в фоне
make docker-logs    # логи сервера и AI-сервиса
make docker-down    # остановить
```

Что делает compose:

- **postgres** — при первом старте применяет `schema/schema.sql`,
  `schema/seed.sql` и миграции из `schema/migrations/` (идемпотентно,
  по алфавиту файлов в `docker-entrypoint-initdb.d`);
- **ai** — образ с `psycopg` (память через PostgreSQL), healthcheck
  `GET /healthz`;
- **server** — образ без компилятора (только `libpq5`), стартует после
  healthy-зависимостей; healthcheck — проверка порта 9000.

Продакшен-чек-лист: смените `AURA_JWT_SECRET`/`AURA_2FA_KEY`/
`POSTGRES_PASSWORD` (без них compose честно поднимется с dev-значениями),
поставьте TLS-терминирующий прокси перед портом 9000 (`wss://`), для
реального push — шлюз APNs (`AURA_PUSH_DRIVER=webhook`), для почты —
`AURA_MAIL_DRIVER=http` + `AURA_EMAIL_API_URL`. Бэкап — `pg_dump` тома
`pgdata`.

## 9. Проверка установки

```bash
make check    # тесты Python + C++
make e2e      # сквозной прогон (серверы должны быть запущены)
python3 tools/check_ios_protocol.py
```

CI (GitHub Actions, `.github/workflows/ci.yml`) на каждый push гоняет
три job'а: сборка C++ + ctest, pytest AI-сервиса, сверка протокола iOS и
синтаксис e2e. Qt/iOS в CI не собираются (нужны Qt 6 и macOS) — это
локальные проверки из [TESTING.md](TESTING.md).

Частые проблемы — [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
