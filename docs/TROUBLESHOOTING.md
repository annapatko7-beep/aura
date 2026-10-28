# Устранение неполадок Selfvoy

## Сервер не запускается

| симптом | причина и решение |
| --- | --- |
| `address already in use` | порт занят: `SELFVOY_PORT=9001 …` или остановите прошлый процесс |
| `не удалось подключиться к PostgreSQL` | проверьте `SELFVOY_DATABASE_URL`; для unix-сокета pgserver: `postgresql://postgres:@/postgres?host=/path/to/pgdata` и `LD_LIBRARY_PATH=<pginstall>/lib` |
| `relation "users" does not exist` | не применена схема: `psql "$SELFVOY_DATABASE_URL" -f schema/schema.sql` + файлы из `schema/migrations/` |
| старт без БД | без `SELFVOY_DATABASE_URL` поднимется встроенное JSON-хранилище (`SELFVOY_EMBEDDED_DB`) — только для разработки |

## Сборка

| симптом | решение |
| --- | --- |
| CMake не находит libpq | укажите явно: `-DPostgreSQL_INCLUDE_DIR=<pg>/include -DPostgreSQL_LIBRARY_DIR=<pg>/lib -DPostgreSQL_LIBRARY=<pg>/lib/libpq.so` |
| Qt-клиент не конфигурируется | нужен Qt ≥ 6.5 c модулями Core, Gui, Network, Qml, Quick, QuickControls2, WebSockets, Multimedia, TextToSpeech; `-DSELFVOY_WITH_CLIENT=ON` |
| хочется проверить QML без Qt-сборки | `qmlcachegen --resource-path "/qt/qml/Selfvoy/<файл>" -I <qmldir-корень> -I <Qt>/qml <файл> -o /tmp/x.qmlc` по каждому файлу (рецепт qmldir — в git-истории этапа 12) |
| Swift не собирается | iOS-проект собирается только на macOS: `xcodegen generate && open Selfvoy.xcodeproj`; без Mac доступен `python3 tools/check_ios_protocol.py` |

## AI-сервис и агент

| симптом | решение |
| --- | --- |
| `upstream_error` в ответе agent.ask | AI-сервис не запущен или неверный `SELFVOY_AI_URL`; в логе сервера будет «AI-сервис не отвечает» |
| AI отвечает 401 | не совпадает `SELFVOY_AI_TOKEN` (сервер) и `SELFVOY_AI_TOKEN` (AI-сервис); заголовок `X-Selfvoy-Token` |
| AI отвечает 503 «token is not configured» | у AI-сервиса не задан `SELFVOY_AI_TOKEN`; задайте общий с сервером токен (`openssl rand -hex 32`) или для локальной разработки `SELFVOY_AI_INSECURE_DEV=1` (`make ai-run`) |
| `docker compose up`: «… is required» | не заполнен секрет в `.env` (см. `.env.example`, `openssl rand -hex 32`) |
| ответы «как заглушка» | это `SELFVOY_LLM_PROVIDER=mock` (по умолчанию); для реального LLM задайте `OPENAI_API_KEY`/`OPENAI_BASE_URL`/`SELFVOY_LLM_MODEL` |
| STT возвращает mock-текст | не задан `SELFVOY_STT_URL`/провайдер; без них — честный mock, не имитация |

## Авторизация

| симптом | решение |
| --- | --- |
| вход отвечает `forbidden` «слишком много попыток» | сработал throttling: подождите `SELFVOY_LOGIN_WINDOW` секунд (по умолчанию 300); счётчики по email и адресу |
| не приходит код подтверждения | при `SELFVOY_MAIL_DRIVER=dev` код возвращается в ответе регистрации и пишется в лог сервера — это режим разработки |
| все сессии внезапно недействительны | сменился `SELFVOY_JWT_SECRET` — токены не проходят; войдите заново (в проде секрет не менять) |
| 2FA-коды не принимаются | рассинхрон часов устройства (окно TOTP ±1 период); потеряли authenticator — используйте резервные коды; сменился `SELFVOY_2FA_KEY` — старые секреты не расшифровать, настройте 2FA заново |

## Интеграции и push

| симптом | решение |
| --- | --- |
| `not_configured` на integrations.begin | не заданы `SELFVOY_GOOGLE_CLIENT_ID`/`SELFVOY_GOOGLE_CLIENT_SECRET` |
| Google отвечает `redirect_uri_mismatch` | в Console должен быть тот же URI: desktop — `http://127.0.0.1:<порт>/callback`, iOS — `selfvoy://oauth` |
| push не приходит на iOS | драйвер `dev` только логирует; для доставки нужны `SELFVOY_PUSH_DRIVER=webhook`, доступный `SELFVOY_PUSH_WEBHOOK_URL` и подписывающий шлюз (SECURITY.md §10); симулятор iOS APNs не получает — нужно устройство |
| уведомления есть в приложении, но не push | так и задумано: in-app работает всегда, push гасят тихие часы/`muted_kinds` (prefs.notifications) |

## E2E-прогон

| симптом | решение |
| --- | --- |
| падают секции OAuth/push | mock-серверы поднимаются самим прогоном: `SELFVOY_E2E_GOOGLE_MOCK=1 SELFVOY_E2E_PUSH_MOCK=1`; сервер должен смотреть `SELFVOY_GOOGLE_*`/`SELFVOY_PUSH_WEBHOOK_URL` на порты mock (9081/9082) |
| `unauthorized` на первом же шаге | сервер запущен с другим `SELFVOY_JWT_SECRET`/БД, чем ожидает прогон; поднимите стек заново (SETUP.md) |
| конфликты данных между прогонами | каждый запуск использует уникальный суффикс `RUN` в email-адресах — не переиспользуйте старый `SELFVOY_E2E_RUN` |

## Куда смотреть

- Лог сервера: `SELFVOY_LOG_LEVEL=info|debug` (структурные строки `[auth]`,
  `[agent]`, `[push]`, `[db]`).
- Журнал безопасности в БД: `audit_logs` (входы, блокировки, 2FA).
- Состояние стека: `server.info` (WS) и `GET /healthz` (AI-сервис).
- Тесты и матрица безопасности: [TESTING.md](TESTING.md).
