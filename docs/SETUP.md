# Установка и настройка Selfvoy

Установка на сервер одной командой (в т.ч. только с телефона) — см.
[DEPLOY.md](DEPLOY.md): `deploy/install.sh`, команда `selfvoy`, TLS через Caddy.

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
createdb selfvoy
psql selfvoy -v ON_ERROR_STOP=1 -f schema/schema.sql
# обновления схемы — идемпотентные файлы из schema/migrations/:
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_09_20_notifications.sql
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_10_13_roles.sql   # роли админ-панели
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_10_14_invites.sql  # приглашения по ссылке
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_10_14_booking_referrals.sql  # запись и рефералы
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_10_14_rate_limits.sql  # общие лимиты
psql selfvoy -v ON_ERROR_STOP=1 -f schema/migrations/2026_10_15_audit_fixes.sql  # ends_at записей, UNLOGGED rate_limits
# после обновления сервера: аккаунты PostgreSQL, обошедшие подтверждение email
# (ошибка до октября 2026) — сначала отчёт, затем -v apply=1 [-v revoke=1]
# (apply=0 — только отчёт). Сбрасывать только при SELFVOY_MAIL_DRIVER=smtp:
# с log код подтверждения до пользователей не дойдёт.
psql selfvoy -f tools/sql/unverified_accounts.sql
# демо-данные (необязательно):
psql selfvoy -f schema/seed.sql
```

Без `SELFVOY_DATABASE_URL` сервер использует встроенное JSON-хранилище
(`SELFVOY_EMBEDDED_DB`) — только для разработки и тестов.

## 3. Python AI Service

```bash
make venv                     # .venv + ai/requirements.txt
# общий с C++-сервером секрет (тот же SELFVOY_AI_TOKEN задайте серверу):
export SELFVOY_AI_TOKEN="$(openssl rand -hex 32)"
cd ai && PYTHONPATH=. uvicorn selfvoy_ai.app:app --host 127.0.0.1 --port 8000
# или для локальной разработки без токена (только 127.0.0.1):
make ai-run                   # = SELFVOY_AI_INSECURE_DEV=1 python -m selfvoy_ai.app
```

Без `SELFVOY_AI_TOKEN` AI-сервис **закрыт**: все эндпоинты, кроме `/healthz`,
отвечают `503` (fail closed). Открыть его без токена можно только явным
`SELFVOY_AI_INSECURE_DEV=1` — для локальной разработки; в продакшене эту
переменную не задавать.

Ключевые переменные (полный список — `ai/selfvoy_ai/config.py`):

| переменная | смысл |
| --- | --- |
| `SELFVOY_AI_TOKEN` | общий секрет C++ ↔ AI (заголовок `X-Selfvoy-Token`); без него — `503` на всё, кроме `/healthz` |
| `SELFVOY_AI_INSECURE_DEV` | `1` — разрешить работу без `SELFVOY_AI_TOKEN` (только локальная разработка; если токен задан, он проверяется всё равно) |
| `SELFVOY_AI_HOST`, `SELFVOY_AI_PORT` | адрес `python -m selfvoy_ai.app` (по умолчанию `127.0.0.1:8000`) |
| `SELFVOY_LLM_PROVIDER` | `mock` (по умолчанию), `openai`, `ollama`, `vllm`, `local` (приватный режим без облака) |
| `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `SELFVOY_LLM_MODEL` | параметры модели (для Ollama: `http://localhost:11434/v1`, модель `llama3.2`) |
| `AURA_FEDERATION_PRIVATE_KEY` | 64-символьный hex Ed25519 приватного ключа узла сети Aura для подписи межагентных пакетов AFP v1.0 |
| `SELFVOY_MAPS_API_URL` | эндпоинт геопоиска (по умолчанию встроенный `/v1/places/search` через OpenStreetMap / Overpass) |
| `SELFVOY_BOOKING_API_URL` / `_WEBHOOK_URL` | эндпоинт бронирования и внешний webhook для отправки брони в рестораны / боты |
| `SELFVOY_MEMORY_BACKEND` | `file` (по умолчанию, `./.selfvoy_memory.json`), `file://путь`, `memory` или `postgres` (DSN — из `SELFVOY_DATABASE_URL`) |
| `SELFVOY_DATABASE_URL` | DSN PostgreSQL для `SELFVOY_MEMORY_BACKEND=postgres` (в compose — роль `selfvoy_ai`) |
| `SELFVOY_RAG_MODE` | `auto`: BM25 всегда, эмбеддинги если заданы |
| `SELFVOY_EMBEDDINGS_URL/_MODEL/_API_KEY` | семантический поиск памяти |
| `SELFVOY_STT_PROVIDER/_URL/_MODEL/_API_KEY` | Whisper-совместимый STT; без них — mock |
| `SELFVOY_CORS_ORIGINS` | для браузерных клиентов |


## 4. C++-сервер

```bash
cmake -S . -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSELFVOY_WITH_LIBPQ=ON -DSELFVOY_BUILD_TESTS=ON -DSELFVOY_WITH_CLIENT=OFF
cmake --build build/server -j
SELFVOY_DATABASE_URL="postgresql://user:pass@host/selfvoy" \
SELFVOY_AI_URL=http://127.0.0.1:8000 \
SELFVOY_AI_TOKEN="$SELFVOY_AI_TOKEN" \
SELFVOY_JWT_SECRET="$(openssl rand -hex 32)" \
SELFVOY_2FA_KEY="$(openssl rand -hex 32)" \
./build/server/server/selfvoy-server
```

Переменные сервера (значения по умолчанию — в `server/include/selfvoy/config.h`).
Прежние имена `AURA_*` (до переименования продукта) сервер и AI-сервис ещё
один релиз читают как запасные, если `SELFVOY_*` не задана, и пишут в лог
предупреждение со списком старых имён:

| группа | переменные |
| --- | --- |
| сеть | `SELFVOY_HOST` (0.0.0.0), `SELFVOY_PORT` (9000), `SELFVOY_MAX_CONNECTIONS`, `SELFVOY_MAX_CONNECTIONS_PER_IP` (20; 0 — без лимита), `SELFVOY_MAX_FRAME`, `SELFVOY_MAX_PREAUTH_FRAME` (64 КБ — кадр до входа), `SELFVOY_AUTH_TIMEOUT` (30 с — соединение без входа закрывается), `SELFVOY_AUTH_RECHECK` (15 с — перепроверка сессии открытого соединения), `SELFVOY_PING_INTERVAL`, `SELFVOY_SOCKET_TIMEOUT` |
| БД | `SELFVOY_DATABASE_URL`, `SELFVOY_EMBEDDED_DB` |
| AI | `SELFVOY_AI_URL`, `SELFVOY_AI_TOKEN`, `SELFVOY_AI_TIMEOUT_MS` |
| секреты | `SELFVOY_JWT_SECRET` — случайная строка ≥ 32 байт (`openssl rand -hex 32`), `SELFVOY_2FA_KEY` (шифр TOTP-секретов и токенов интеграций). Без них, со слабым/шаблонным секретом (`selfvoy-dev-secret`, `CHANGE-IN-PRODUCTION…`, < 32 байт) сервер **не стартует** |
| режим разработки | `SELFVOY_DEV_MODE=1` — только локально: разрешает `SELFVOY_MAIL_DRIVER=dev` и запуск без секретов (JWT-секрет генерируется случайно на время жизни процесса — токены не переживают перезапуск) |
| токены/лимиты | `SELFVOY_TOKEN_TTL` (7 дней), `SELFVOY_REFRESH_TTL` (30 дней), `SELFVOY_VERIFY_TTL`, `SELFVOY_RESET_TTL` (≤ 1 ч), `SELFVOY_MAX_LOGIN_ATTEMPTS` (8 на email), `SELFVOY_MAX_IP_ATTEMPTS` (30 неудач с IP), `SELFVOY_LOGIN_WINDOW` (300 с), `SELFVOY_MAX_2FA_ATTEMPTS` (5 неверных TOTP на аккаунт) / `SELFVOY_2FA_WINDOW` (900 с), `SELFVOY_MAX_CODE_ATTEMPTS` (5 — затем код подтверждения/сброса сгорает), `SELFVOY_MAIL_PER_EMAIL_HOUR` (3 письма на адрес), `SELFVOY_MAIL_PER_IP_HOUR` (20), `SELFVOY_REGISTER_PER_IP_HOUR` (10; 0 — без лимита) |
| почта | `SELFVOY_MAIL_DRIVER` (по умолчанию `log`): `smtp` — письма с кодами (`SELFVOY_SMTP_HOST/PORT/USER/PASSWORD/FROM/TLS`, `SELFVOY_SMTP_CA_FILE`), `log` — без отправки, в лог пишется только факт выдачи кода, `dev` — код в ответе API (только вместе с `SELFVOY_DEV_MODE=1`, иначе сервер не стартует) |
| Google OAuth | `SELFVOY_GOOGLE_CLIENT_ID/_SECRET`, `SELFVOY_GOOGLE_AUTH_URL/_TOKEN_URL/_REVOKE_URL/_CALENDAR_URL/_GMAIL_URL`, `SELFVOY_OAUTH_STATE_TTL` |
| push | `SELFVOY_PUSH_DRIVER` (`dev`/`webhook`), `SELFVOY_PUSH_WEBHOOK_URL`, `SELFVOY_APNS_TOPIC` (app.selfvoy), `SELFVOY_APNS_URL`, `SELFVOY_FCM_URL` (FCM HTTP v1, этап 11: замените `selfvoy` на id проекта Firebase) |
| админ-панель | `SELFVOY_ADMIN_EMAILS` — email первых администраторов через запятую; роль `admin` выдаётся только подтверждённому адресу при старте сервера и при входе (см. PROTOCOL.md → «Администрирование»). Нужна миграция `schema/migrations/2026_10_13_roles.sql` |
| приглашения по ссылке | `SELFVOY_PUBLIC_URL` — адрес веб-клиента (`https://selfvoy.example.com`): `invite.create` вернёт готовую ссылку `…/#/i/<токен>`; без него ссылку собирает клиент (в браузере на своём домене) или показывает только токен (Electron/Capacitor/localhost). Нужна миграция `schema/migrations/2026_10_14_invites.sql` |
| страницы записи и рефералы | тот же `SELFVOY_PUBLIC_URL`: `booking.pageCreate` вернёт `…/#/b/<slug>`, `booking.book` — ссылку управления `…/#/m/<токен>`, `referral.get` — `…/#/signup?ref=<код>`. Нужны AI-сервис (подбор свободного времени) и миграция `schema/migrations/2026_10_14_booking_referrals.sql` |
| тариф Pro | `SELFVOY_PLANS_ENFORCED` (0 по умолчанию — бета, функции Pro открыты всем; 1 — нужен действующий Pro, иначе `pro_required`). Pro выдаёт администратор (`admin.setPlan`) или команда `selfvoy pro EMAIL ДНЕЙ`. Нужна миграция `schema/migrations/2026_10_16_telegram_pro.sql` |
| Telegram-бот | `SELFVOY_TELEGRAM_BOT_TOKEN` (от @BotFather), `SELFVOY_TELEGRAM_BOT_USERNAME` (без @) — бот включён, когда заданы оба; `SELFVOY_TELEGRAM_API_URL` (по умолчанию `https://api.telegram.org`), `SELFVOY_TELEGRAM_POLL` (0 — только отправка). Опрос `getUpdates` ведёт один экземпляр (advisory lock PostgreSQL), webhook не нужен. Сервер должен быть собран с OpenSSL (https) |
| исходящий https | Сборка с OpenSSL (`libssl-dev`): https-запросы (Telegram, Google) идут напрямую — TLS ≥ 1.2, проверка сертификата и имени хоста; `SELFVOY_CA_FILE` — свой корневой сертификат. Без OpenSSL https-запрос возвращает ошибку «сборка без TLS» |
| ICS-подписка и пост окон (F6) | `SELFVOY_SERVER_PUBLIC_URL` — внешний адрес самого сервера (`https://api.example.com`, тот же хост и порт, что у `wss://`): `calendar.feedReset` вернёт готовую ссылку `…/ics/<токен>.ics`; без него `url` пуст, клиент собирает адрес из адреса сервера. `GET /ics/…` и `GET /healthz` отвечает тот же порт, что WebSocket, — TLS-прокси должен пропускать к нему и обычные HTTP-запросы. `booking.slotsPost` берёт ссылку страницы из `SELFVOY_PUBLIC_URL`. Нужна миграция `schema/migrations/2026_10_16_ics_feed.sql` |
| цена, предоплата, ЮKassa (F4) | Владелец подключает свой магазин ЮKassa сам (`payments.connect`: `shop_id` и секретный ключ из личного кабинета ЮKassa → «Интеграция» → «Ключи API»; ключ хранится зашифрованным ключом `SELFVOY_2FA_KEY`). Серверу нужны `SELFVOY_PUBLIC_URL` (адрес возврата гостя после оплаты `…/#/m/<токен>?paid=1`) и сборка с OpenSSL; `SELFVOY_YOOKASSA_API_URL` — адрес API (по умолчанию `https://api.yookassa.ru/v3`, в e2e — http-мок). Статус платежей проверяет планировщик (`SELFVOY_SCHEDULER_INTERVAL_MS`, не чаще раза в 30 с на платёж) — webhook ЮKassa не нужен. Чеки 54-ФЗ сервер не отправляет: самозанятый формирует чек в «Мой налог», ИП/организация — настройками магазина в ЮKassa. Без ЮKassa работает оплата по ссылке/реквизитам владельца. Нужна миграция `schema/migrations/2026_10_16_payments.sql` |
| прочее | `SELFVOY_SCHEDULER_INTERVAL_MS` (0 — только ручной tick), `SELFVOY_TOOLS_MODE`, `SELFVOY_MAPS_API_URL`, `SELFVOY_LOG_LEVEL` |

Лимиты по IP считаются по адресу без порта, для IPv6 — по сети /64 (смена
адреса внутри своей сети их не обходит). Если счётчики `rate_limits` в
PostgreSQL недоступны (нет прав, таймаут), лимит считается исчерпанным — защита
от перебора не отключается молча. Если сервер стоит за обратным
прокси, задайте `SELFVOY_TRUSTED_PROXIES` — IP прокси через запятую: от них сервер
берёт адрес клиента из `X-Forwarded-For` (правый адрес, не принадлежащий
доверенным прокси; от остальных заголовок игнорируется), и лимиты попыток
входа, регистрации и гостевых страниц считаются по клиенту. Лимит соединений
`SELFVOY_MAX_CONNECTIONS_PER_IP` проверяется до рукопожатия — по адресу прокси, его
за прокси поднимите или отключите (0).

Локальный запуск без секретов и SMTP (код подтверждения — прямо в ответе API):

```bash
SELFVOY_DEV_MODE=1 SELFVOY_MAIL_DRIVER=dev ./build/server/selfvoy-server
```

Сервер ходит во внешние HTTP API без TLS: при `SELFVOY_GOOGLE_CLIENT_ID` и
https-адресах `SELFVOY_GOOGLE_TOKEN_URL/REVOKE_URL/CALENDAR_URL/GMAIL_URL` в лог при
старте пишется предупреждение — направьте их на http-адрес TLS-прокси (egress).

### Почта (коды подтверждения и сброса пароля)

Сервер сам отправляет письма по SMTP (OpenSSL, TLS 1.2+, проверка сертификата
и имени хоста). Подойдёт любой почтовый сервис с SMTP: Яндекс 360, Mail.ru для
бизнеса, Google Workspace, Amazon SES, Mailgun, Postmark, SendGrid.

```env
SELFVOY_MAIL_DRIVER=smtp
SELFVOY_SMTP_HOST=smtp.yandex.ru      # адрес SMTP провайдера
SELFVOY_SMTP_PORT=587                 # 587 + starttls или 465 + tls
SELFVOY_SMTP_TLS=starttls
SELFVOY_SMTP_USER=no-reply@example.com
SELFVOY_SMTP_PASSWORD=<пароль приложения>
SELFVOY_SMTP_FROM=Selfvoy <no-reply@example.com>
```

- Письма: «Код подтверждения Selfvoy: 123456» (код в теме, тексте и HTML) и
  «Сброс пароля Selfvoy». Код в ответе API не возвращается.
- Без TLS (`SELFVOY_SMTP_TLS=none`) авторизация разрешена только к ретранслятору
  на этой же машине (`localhost`).
- Ошибки доставки пишутся в лог (`[mail] … не отправлено …: причина`); пользователь
  может запросить код ещё раз.
- Для доставляемости настройте у домена SPF, DKIM и DMARC (у провайдера).
- Проверка локально: `aiosmtpd` со STARTTLS и своим CA (`SELFVOY_SMTP_CA_FILE`).

### Google OAuth (интеграции Calendar/Gmail)

1. Google Cloud Console → OAuth Client (Web application).
2. Redirect URI: desktop — `http://127.0.0.1:<порт>/callback` (клиент
   поднимает loopback-сервер сам); iOS — `selfvoy://oauth`.
3. Scopes: `calendar.events.readonly`, `gmail.send`.
4. Заполните `SELFVOY_GOOGLE_*`; без client_id интеграции отвечают
   `not_configured` (честно, без заглушек).

### Push (APNs через шлюз)

Ядро не делает TLS/HTTP2/ES256 — это задача внешнего шлюза:

```
SELFVOY_PUSH_DRIVER=webhook
SELFVOY_PUSH_WEBHOOK_URL=https://push-gateway.internal/push
```

Сервер POST-ит шлюзу конверт `{platform, token, notification_id, kind,
apns: {url, headers, payload}}`; шлюз подписывает JWT ключом `.p8` и
форвардит `apns.payload` на `apns.url`. Драйвер `dev` логирует доставки.

## 5. Desktop-клиент (Qt)

```bash
cmake -S . -B build/client -G Ninja -DSELFVOY_WITH_CLIENT=ON
cmake --build build/client -j
./build/client/client/selfvoy-client
```

Адрес сервера задаётся в окне входа (сохраняется в QSettings вместе с
токенами). Для продакшена — `wss://` за TLS-прокси.

### Готовый .exe для Windows — через GitHub Actions

Локальная сборка не обязательна: workflow **`build-windows.yml`** на каждый
push собирает `selfvoy-client.exe` с Qt-рантаймом и libpq (Windows Server 2022,
Qt 6.7 MSVC). Скачать: вкладка **Actions** → «Build Windows client» →
последний запуск → **Artifacts → selfvoy-desktop-windows** (zip). Это релизная
сборка без подписи кода; для распространения подпишите exe своим
сертификатом и/или соберите установщик (NSIS/MSI).

## 6. iOS-клиент

```bash
brew install xcodegen
cd ios && xcodegen generate && open Selfvoy.xcodeproj
```

- В схеме Selfvoy включите Push Notifications (entitlement `aps-environment`
  уже в `project.yml`) и свой bundle id `app.selfvoy`.
- Адрес сервера — в поле входа (`wss://…` для ATS; `ws://` только для
  локальной разработки с исключением в Info.plist).
- Siri/Команды: App Intents регистрируются при первом запуске; Back Tap
  настраивается пользователем в системных настройках через Команду —
  приложение честно показывает пошаговую инструкцию (см. MOBILE_FEATURES).

## 7. 2FA (TOTP)

Работает из коробки: `auth.setup2fa` → QR/otpauth URI → `auth.confirm2fa`
→ 10 резервных кодов. Секреты шифруются ключом `SELFVOY_2FA_KEY` (fallback —
`SELFVOY_JWT_SECRET`); в продакшене задайте отдельный `SELFVOY_2FA_KEY` и не
меняйте его (иначе существующие секреты не расшифруются).

## 8. Production-стек через Docker Compose

Весь серверный стек (PostgreSQL 16 + AI Service + C++-сервер) поднимается
одной командой; Qt-клиент собирается на хосте (нужен GUI), iOS — на Mac.

```bash
cp .env.example .env      # .env в git не коммитить (он в .gitignore)
# Заполните ВСЕ секреты, каждый — openssl rand -hex 32:
#   SELFVOY_JWT_SECRET  SELFVOY_2FA_KEY  SELFVOY_AI_TOKEN
#   POSTGRES_PASSWORD  SELFVOY_DB_PASSWORD  SELFVOY_AI_DB_PASSWORD
# + по необходимости: SELFVOY_MAIL_DRIVER=smtp и SELFVOY_SMTP_*, OPENAI_API_KEY,
#   SELFVOY_LLM_PROVIDER=openai, SELFVOY_PUSH_DRIVER=webhook SELFVOY_PUSH_WEBHOOK_URL=…,
#   SELFVOY_GOOGLE_CLIENT_ID/_SECRET

make docker-build   # собрать образы (server: multi-stage CMake+libpq; ai: python:3.12-slim)
make docker-up      # поднять стек в фоне
make docker-logs    # логи сервера и AI-сервиса
make docker-down    # остановить
```

Что делает compose:

- **postgres** — при первом старте пустого тома применяет только
  `schema/schema.sql` (уже включает все миграции) и
  `deploy/postgres/20-app-roles.sh`, который создаёт роли приложения.
  `schema/seed.sql` (демо-аккаунты) в прод **не** монтируется. Порт 5432
  наружу не публикуется; обслуживание — `docker compose exec postgres psql -U postgres selfvoy`.
  Новые миграции применяйте вручную под `postgres`
  (`docker compose exec -T postgres psql -U postgres selfvoy < schema/migrations/<файл>.sql`):
  права `selfvoy_app` на новые таблицы выдаются автоматически (default privileges).
- **ai** — образ с `psycopg`, пользователь без root (uid 10001). Порт 8000
  наружу не публикуется — к сервису ходит только `server` по сети compose с
  `X-Selfvoy-Token`. Healthcheck — `GET /healthz` средствами Python (curl в
  slim-образе нет).
- **server** — образ без компилятора (только `libpq5`), без root; стартует
  после healthy-зависимостей; healthcheck — проверка порта 9000. Наружу
  публикуется только 9000 (`SELFVOY_BIND=127.0.0.1`, если TLS-прокси на этой же машине).

### Безопасность развёртывания

| что | как устроено |
| --- | --- |
| секреты | `${VAR:?}` в `docker-compose.yml`: без `SELFVOY_JWT_SECRET`, `SELFVOY_2FA_KEY`, `SELFVOY_AI_TOKEN`, `POSTGRES_PASSWORD`, `SELFVOY_DB_PASSWORD`, `SELFVOY_AI_DB_PASSWORD` стек не стартует; в `.env.example` значения пустые. Генерация — `openssl rand -hex 32` (hex безопасен внутри URL БД) |
| роли БД | `postgres` — только инициализация и миграции; `selfvoy_app` (C++-сервер) — `SELECT/INSERT/UPDATE/DELETE`, без DDL и суперпользователя; `selfvoy_ai` (AI-сервис) — только `user_memory` и `users(id, email)` |
| AI-сервис | fail closed без `SELFVOY_AI_TOKEN` (`503`); `/healthz` не раскрывает DSN и пути; ошибки `500` без текста исключения; инструменты с внешним эффектом (`send_email`, `send_message`, `book_table`) исполняет только C++-сервер после `tool_permissions` и подтверждения |
| режим разработки | `SELFVOY_DEV_MODE` и `SELFVOY_AI_INSECURE_DEV` в продакшене **не задавать**; почта в compose по умолчанию `SELFVOY_MAIL_DRIVER=log` (для писем — `smtp` + `SELFVOY_SMTP_*`) |
| контейнеры | `ai` и `server` работают от непривилегированного пользователя |

Продакшен-чек-лист: заполните все секреты (см. выше), поставьте
TLS-терминирующий прокси перед портом 9000 (`wss://`), для реального push —
шлюз APNs (`SELFVOY_PUSH_DRIVER=webhook`), для почты — `SELFVOY_MAIL_DRIVER=smtp` +
`SELFVOY_SMTP_*` (см. «Почта» выше). Бэкап — `pg_dump` тома `pgdata`.
Сменить пароли ролей на существующем томе: `ALTER ROLE selfvoy_app PASSWORD '…'`
(init-скрипты выполняются только при создании тома).

## 9. Проверка установки

```bash
make check    # тесты Python + C++
make e2e      # сквозной прогон (серверы должны быть запущены)
python3 tools/check_ios_protocol.py
```

CI (GitHub Actions, `.github/workflows/ci.yml`) на каждый push гоняет
сборку C++ + ctest, pytest AI-сервиса, сверку протокола iOS/Android,
синтаксис e2e и JVM-тесты SelfvoyKit. Сборочные job'ы работают с токеном
только на чтение (`persist-credentials: false`); запись (ветки `builds/*`,
релиз «builds») — отдельными job'ами публикации и только для push в `main`,
логи — в `arena/build-logs` на любом push. Qt/iOS в CI не собираются (нужны Qt 6 и macOS) — это
локальные проверки из [TESTING.md](TESTING.md).

Частые проблемы — [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Несколько экземпляров сервера

На общем PostgreSQL можно запускать несколько `selfvoy-server` за балансировщиком
(WebSocket — с привязкой клиента к экземпляру на время соединения):

- счётчики попыток (вход, коды из писем, 2FA, лимиты `invite.*`/`booking.*`)
  общие — таблица `rate_limits`; перебор «по кругу» через экземпляры не
  обходит лимиты;
- запись встречи (`invite.respond`, `booking.book/reschedule/cancel`) идёт под
  advisory lock'ом владельца — два экземпляра не займут одно время;
- отзыв сессий на другом экземпляре закрывает соединения не мгновенно, а при
  перепроверке сессии (`SELFVOY_AUTH_RECHECK`, по умолчанию 15 с);
- кэш свободного времени страниц записи — свой у каждого экземпляра (до
  минуты); правильность записи от него не зависит: занятость проверяется по БД.

Встроенное хранилище (без `SELFVOY_DATABASE_URL`) — только для одного процесса.
