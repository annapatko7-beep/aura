# Selfvoy — персональный ИИ-агент (монорепо)

Кроссплатформенное приложение: C++17 сервер (WebSocket + HTTP), Python AI-сервис
(FastAPI, LLM/RAG/память), Qt6/QML desktop-клиент, Android (Kotlin + Compose),
iOS (SwiftUI). База — PostgreSQL, 34 таблицы и 2 представления (`schema/schema.sql` + `schema/migrations/`).

## Команды (всё через Makefile)

| Команда | Что делает |
|---|---|
| `make check` | полный прогон: pytest (AI) + ctest (C++) + сверка протокола |
| `make ai-test` | тесты Python AI-сервиса (`cd ai && pytest`) |
| `make server-build` / `make server-test` | сборка и юнит-тесты C++-сервера |
| `make protocol-check` | сверка типов сообщений iOS/Android с сервером (`tools/check_*_protocol.py`) |
| `make e2e` | сквозной сценарий `tools/e2e.py` (нужны запущенные AI :8000 и сервер :9000) |
| `make llm-check` | отвечает ли настоящая LLM (`OPENAI_API_KEY` в окружении) |
| `make schema-check` | применить `schema/schema.sql` + `seed.sql` к PostgreSQL |
| `make docker-up` / `docker-down` / `docker-logs` | прод-стек (postgres + ai + server) |

Перед любым «готово» — минимум `make check`; если менялся протокол — обязательно
`make protocol-check`; если менялись серверные сценарии — `make e2e`.

## Структура

- `server/` — C++17 сервер: WS-протокол, Argon2id, JWT, TOTP (2FA), Google OAuth,
  задачи/уведомления/память, FCM/APNs/webhook-пуш. **POSIX-only** (сокеты, poll):
  на Windows не собирается и не должен — см. `SELFVOY_WITH_SERVER`.
- `ai/` — Python 3.11+ FastAPI: LLM-оркестрация, переговоры агентов, RAG, память.
- `client/` — Qt6/QML desktop-клиент (Core, Network, Qml, Quick, QuickControls2,
  WebSockets, Multimedia, TextToSpeech). Серверный код НЕ линкует.
- `android/` — два модуля: `selfvoykit` (чистый JVM, протокол — тестируется без SDK)
  и `app` (Compose-клиент, пакет `app.selfvoy`).
- `ios/` — SwiftUI-клиент.
- `schema/` — схема БД и миграции; `docs/` — PROTOCOL.md, DATABASE.md, SETUP.md, ANDROID.md.
- `tools/e2e.py` — эталонный клиент протокола на Python (456 проверок со всеми моками).
- `.github/workflows/` — `ci.yml` (тесты), `build-windows.yml` (.exe), `build-android.yml` (.apk).

## Протокол (главные грабли — читать перед правками)

- Клиент → сервер: `{"id":int, "type":"<домен>.<метод>", "payload":{...}}`.
- Ответ: `{"id", "type":"ok"|"error", "code", "message", "payload"}`;
  событие: `{"type":"event", "event":"chat.message"|"task.due"|..., "payload"}`.
- `chat.send` / `chat.history` — поле **`body`** (не `text`).
- `prefs.set` — поля в **корне** payload (не обёрткой `{"preferences":...}`).
- `chat.open` → payload `{chat:{id}}`.
- `memories.kind` — только `fact|preference|schedule|contact`.
- Публичные (без входа) методы гостя: `invite.get/respond`, `booking.get/book/manage/
  reschedule/cancel/resendLink` — веб-страницы `#/i/<токен>`, `#/b/<slug>`, `#/m/<токен>`; токены
  ссылок в БД только sha256. Рефералы: `referral.get`, `auth.register` + `ref`.
- Deep link `selfvoy://`: `chats/<id>`, `voice`, `ask?text=`, `tasks`, `notifications`,
  `settings`, `oauth?provider=&code=&state=`, `signup?ref=` (Android/iOS).
- Смена протокола = правка сервера + всех клиентов + `docs/PROTOCOL.md` + чекеры
  в `tools/` + `tools/e2e.py` в одном коммите.

## Жёсткие правила продукта (нельзя нарушать)

- Без SMS/телефонной авторизации; 2FA — только TOTP.
- Никакой фейковой регистрации, хардкод-паролей, тестовых токенов в коде.
- LLM — не источник истины: сервер валидирует JSON/ограничения, опасные операции
  только через подтверждение пользователя; переговоры агентов — максимум 5 раундов,
  затем эскалация; `tool_permissions` проверяются до действия.
- Пароли: минимум 8 символов, буквы+цифры, Argon2id.
- Секреты — только через окружение (`.env`, шаблон `.env.example`); в git их не класть.

## Сборка артефактов (.exe / .apk)

Локально в песочницах обычно не собрать (нужны MSVC/Qt и Android SDK) — это
делает GitHub Actions:
- **.exe**: Actions → «Build Windows client» → Artifacts → `selfvoy-desktop-windows`
  (собирается только клиент: `-DSELFVOY_WITH_CLIENT=ON -DSELFVOY_WITH_SERVER=OFF`, MSVC,
  Qt ставится через `aqtinstall`).
- **.apk**: Actions → «Build Android APK» → Artifacts → `selfvoy-android-debug`
  (debug-подпись; для Play нужен свой keystore и `google-services.json` для FCM).
- Логи упавших шагов CI дублирует аннотациями прогона и в ветку `arena/build-logs`.

## Локальный запуск стека

1. `make schema-check` (нужен PostgreSQL; URI — `PG_URI`).
2. `make ai-run` → AI на :8000 (env: `SELFVOY_MEMORY_BACKEND`, `SELFVOY_RAG_MODE=auto`).
3. `make server-run` → сервер на :9000 (env: `SELFVOY_DATABASE_URL`, `SELFVOY_AI_URL`,
   `SELFVOY_JWT_SECRET`, `SELFVOY_2FA_KEY`, `SELFVOY_MAIL_DRIVER=dev`).
4. `make e2e` (с `SELFVOY_E2E_GOOGLE_MOCK=1 SELFVOY_E2E_PUSH_MOCK=1` — моки OAuth/пуша).

## Стиль и проверки

- После правки файлов с русским текстом прогони CJK-скан:
  `grep -nP '[\x{3040}-\x{30ff}\x{3400}-\x{4dbf}\x{4e00}-\x{9fff}]' <файлы>` —
  редакторы иногда вставляют иероглифы в комментарии; их быть не должно.
- QML можно проверить без GUI: `qmlcachegen` из PySide6 по файлам `client/qml/`.
- В Kotlin `data class ... : Exception()` нельзя называть поле `message`
  (скрывает `Throwable.message`); внутри файлов с собственным sealed `Result`
  используй `kotlin.Result.success/failure` явно.
- Общение с пользователем — на русском; отчёт по этапу:
  done / files / working / to-check / limits / next.