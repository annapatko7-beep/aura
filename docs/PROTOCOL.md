# Протокол Selfvoy

Транспорт — WebSocket (RFC 6455) поверх TCP. Все сообщения — JSON в текстовых
фреймах. Один TCP-сокет = одна `Session` на сервере.

Клиенты: Qt/QML (desktop), Swift/SwiftUI (iOS, этап 10 — см. docs/IOS.md),
Kotlin/Compose (Android, этап 11 — см. docs/ANDROID.md), Python (e2e).
Протокол один на всех; сверка имён обработчиков мобильных клиентов —
`tools/check_ios_protocol.py` и `tools/check_android_protocol.py`.

## Конверт

```jsonc
// запрос клиента
{ "id": "req-1", "type": "chat.send", "payload": { ... } }

// ответ сервера
{ "id": "req-1", "type": "ok",    "payload": { ... } }
{ "id": "req-1", "type": "error", "code": "forbidden", "message": "..." }

// push-событие сервера (без id)
{ "type": "event", "event": "chat.message", "payload": { ... } }
```

`id` — строка или число; сервер возвращает его в том же виде (число — числом).

Авторизация двумя способами:

1. заголовок `Authorization: Bearer <jwt>` в HTTP-рукопожатии — сервер сразу
   считает сессию авторизованной и шлёт событие `session.ready`;
2. сообщение `auth.login` / `auth.token` уже после подключения.

Ограничения соединения: до входа кадр не больше 64 КБ (`SELFVOY_MAX_PREAUTH_FRAME`,
больше — закрытие 1009/1002); соединение без входа закрывается через 30 с
(`SELFVOY_AUTH_TIMEOUT`, код закрытия 4408); с одного IP — не больше 20 соединений
(`SELFVOY_MAX_CONNECTIONS_PER_IP`, лишнее получает HTTP 429 на рукопожатии). В JSON-объекте
не больше 1000 ключей.

Открытое соединение не переживает свою сессию: при отзыве сессии
(`sessions.revoke`/`revokeAll`, `auth.logout` с тем же токеном на другом сокете),
смене/сбросе пароля, повторном использовании refresh-токена или блокировке
аккаунта сервер шлёт событие `session.revoked` (`{reason}`) и закрывает сокет с
кодом 4401. Кроме того, сессия соединения сверяется с БД не реже чем раз в 15 с
(`SELFVOY_AUTH_RECHECK`): отозванная/истёкшая → `unauthorized` «сессия истекла или
отозвана — войдите снова», соединение разлогинивается.

Все типы, кроме `auth.register`, `auth.login`, `auth.login2fa`, `auth.verifyEmail`,
`auth.resendCode`, `auth.forgotPassword`, `auth.resetPassword`,
`auth.refresh`, `ping`, `server.info`, `invite.get`, `invite.respond`,
`booking.get`, `booking.book`, `booking.manage`, `booking.reschedule`,
`booking.cancel`, `booking.resendLink`, `booking.confirm`, `booking.paymentClaim`,
`package.get`, `package.paymentClaim`,
требуют авторизации.

## Коды ошибок

| code | смысл |
| --- | --- |
| `bad_request` | неизвестный тип, пустое поле, некорректный JSON, код подтверждения/сброса неверен |
| `unauthorized` | нет токена, токен недействителен, неверный пароль, refresh-токен отозван/истёк |
| `email_not_verified` | пароль верный, но email не подтверждён — нужен `auth.verifyEmail` |
| `requires_2fa` | пароль верный, но включена 2FA — нужен `auth.login2fa` с кодом |
| `forbidden` | нет доступа к чату (в т.ч. `agent.ask` с чужим `chat_id`), аккаунт отключён, слишком много попыток, инструмент запрещён (`deny`) |
| `not_found` | пользователь/чат не найдены |
| `conflict` | email уже занят |
| `upstream_error` | AI-сервис недоступен или вернул мусор |
| `internal_error` | необработанное исключение; текст всегда обобщённый («внутренняя ошибка сервера»), подробности (SQL и т.п.) — только в логе сервера |
| `pro_required` | действие доступно только на тарифе Pro: сервер запущен с `SELFVOY_PLANS_ENFORCED=1`, а Pro у пользователя не действует (`user.plan.name = "free"`). При `SELFVOY_PLANS_ENFORCED=0` (бета, по умолчанию) не возвращается |

## Методы

### Авторизация

Поток аккаунта (auth v3): регистрация → код подтверждения email → вход.
Регистрация сессию не выдаёт. В каждом `user` есть `role`
(`user`|`moderator`|`admin`, см. «Администрирование») и тариф
`plan: {name: "pro"|"free", until: ISO|"", enforced: bool}` — `name` честный
(Pro, пока `until` в будущем) независимо от `enforced`; `enforced` —
`SELFVOY_PLANS_ENFORCED` (false — бета: функции Pro открыты всем; true — без
действующего Pro они отвечают `pro_required`). Pro выдаёт администратор
(`admin.setPlan`) или команда деплоя `selfvoy pro EMAIL ДНЕЙ`.

| тип | payload | ответ |
| --- | --- | --- |
| `auth.register` | `display_name`, `email`, `password`, `ref?` (реферальный код, 8 букв/цифр, регистр не важен), `ref_source?` (`link`/`booking`/`invite`) | `requires_verification`, `user`, `email_code` (только `SELFVOY_MAIL_DRIVER=dev` + `SELFVOY_DEV_MODE=1`); занятый email → `conflict`; больше 10 регистраций с IP в час → `forbidden`. Неверный `ref` молча игнорируется |
| `auth.verifyEmail` | `email`, `code` (6 цифр) | `verified`, `user` (повтор для уже подтверждённого адреса — только `verified`, без данных пользователя); после 5 неверных кодов код сгорает (нужен `auth.resendCode`) |
| `auth.resendCode` | `email` | `message` (+`email_code` в dev); ответ не раскрывает существование email; не больше 3 писем на адрес в час (сверх лимита ответ тот же, письмо не уходит) |
| `auth.login` | `email`, `password`, `device?`, `device_id?` | `token`, `token_type`, `expires_in`, `refresh_token`, `refresh_expires_in`, `session_id`, `user`; до подтверждения email — `email_not_verified`; при включённой 2FA и недоверенном `device_id` — `requires_2fa`; отключённый аккаунт — `forbidden` «аккаунт отключён» (только при верном пароле) |
| `auth.login2fa` | `email`, `password`, `code`, `trust_device?`, `device_id?`, `device?` | как `auth.login`; `code` — 6-значный TOTP или резервный `XXXXX-XXXXX`; `trust_device` помечает устройство доверенным на 90 дней; 5 неверных кодов за 15 мин → `forbidden` до конца окна (счётчик на аккаунт, верный пароль его не сбрасывает) |
| `auth.refresh` | `refresh_token`, `device?` | новая пара `token`+`refresh_token` (ротация); текущее соединение авторизуется новой сессией, как после `auth.login` |
| `auth.token` | `token` | `user` |
| `auth.me` | — | `user` + `preferences` |
| `auth.changePassword` | `old_password`, `new_password` | `changed` + новая пара для текущего устройства, как у `auth.login`: `token`, `token_type`, `expires_in`, `refresh_token`, `refresh_expires_in`, `session_id`, `user`. Все прежние сессии и refresh-токены (включая текущие) отозваны — клиент сохраняет новую пару; соединение продолжает работать под новой сессией, остальные соединения пользователя закрываются (`session.revoked`) |
| `auth.forgotPassword` | `email` | `message` (+`reset_code` в dev — **6 цифр**); ответ всегда «ок»; не больше 3 писем на адрес в час |
| `auth.resetPassword` | `email`, `code` (6 цифр, живёт 1 ч), `new_password` | `reset`; после 5 неверных кодов код сгорает; все сессии и refresh-токены отзываются, открытые соединения закрываются |
| `auth.logout` | — | `revoked`; отзывает сессию и refresh-токены этого устройства |
| `profile.update` | `display_name?` (1–80 симв.), `bio?` (≤300 симв.), `avatar_url?` (пусто, `https://…` или `data:image/{png,jpeg,webp};base64,…` до ~100 КБ) | `user`; меняются только переданные поля; пустой payload или неверное значение → `bad_request` |

### Сессии

| тип | payload | ответ |
| --- | --- | --- |
| `sessions.list` | — | `sessions[]`: `id`, `device`, `remote_addr`, `created_at`, `expires_at`, `current` |
| `sessions.revoke` | `session_id` | `revoked`; отзывает и refresh-токены этой сессии, её открытые соединения закрываются; при отзыве своей сессии соединение разлогинивается |
| `sessions.revokeAll` | — | `revoked`; текущая сессия и её refresh-токен остаются живыми, все прочие сессии и refresh-токены отозваны, их соединения закрыты |

### Двухфакторная аутентификация (TOTP)

Порядок входа: пароль → TOTP → сессия. Секрет TOTP хранится зашифрованным
(AES-256-GCM, ключ — `SELFVOY_2FA_KEY`). Настройка: `auth.setup2fa` выдаёт секрет
(base32) и `otpauth://`-URI для приложения-аутентификатора (Google Authenticator,
1Password и др.); `auth.confirm2fa` проверяет одноразовый код, включает 2FA и
возвращает 10 резервных кодов (показываются один раз). RFC 6238: 6 цифр, период
30 с, HMAC-SHA1, окно ±1 период, код одноразовый.

| тип | payload | ответ |
| --- | --- | --- |
| `auth.setup2fa` | — | `secret` (base32), `otpauth_uri`, `issuer`, `algorithm`, `digits`, `period`; 2FA остаётся выключенной до подтверждения; если 2FA уже включена → `bad_request` (сначала `auth.disable2fa`) |
| `auth.confirm2fa` | `code` | `recovery_codes[]` (10 шт., формат `XXXXX-XXXXX`, одноразовые); включает 2FA |
| `auth.status2fa` | — | `enabled`, `pending`, `recovery_codes_left` |
| `auth.disable2fa` | `password` | `ok`; отключение только после повторной проверки пароля; удаляет секрет, резервные коды и доверенные устройства |
| `devices.list` | — | `devices[]`: `id`, `device`, `remote_addr`, `created_at`, `expires_at`, `last_used_at` |
| `devices.revoke` | `id` | `ok`; отзывает одно доверенное устройство |
| `devices.revokeAll` | — | `ok`; отзывает все доверенные устройства |

Резервные коды хранятся как SHA-256 в `recovery_codes` (одноразовые). Доверенные
устройства (90 дней) — в `trusted_devices`; вход с доверенным `device_id`
пропускает ввод кода. События 2FA (`2fa_enabled`, `2fa_disabled`, `2fa_failed`,
`login_2fa_required` и др.) пишутся в `audit_logs`.

Пароль: минимум 8 символов (кодовых точек UTF-8), хотя бы одна буква (латиница,
в т.ч. Latin-1/расширенная, греческий, кириллица — «пароль12» допустим) и цифра,
не больше 256 байт. Хранится как
`argon2id$v=19$m=65536,t=3,p=4$<соль>$<хэш>` (Argon2id, RFC 9106). Старые
хэши `pbkdf2$…` проверяются и при первом успешном входе прозрачно
пересчитываются в Argon2id.
Access-токен: JWT HS256 с claims `sub`, `email`, `jti`, `iat`, `exp`; `jti`
сверяется с таблицей `sessions`, поэтому выход действительно отзывает токен.
`sub` должен совпадать с владельцем сессии `jti`, аккаунт должен быть активен.
Refresh-токен: случайные 32 байта, в БД хранится SHA-256; ротация по
«семействам» — повторное использование токена, уже заменённого при ротации
(в т.ч. два одновременных `auth.refresh` одним токеном), отзывает всё
семейство и все сессии пользователя (защита от кражи). Токен, отозванный
выходом/отзывом сессии, просто `unauthorized`.
Одноразовые коды (подтверждение email, сброс пароля) — 6 цифр, хранятся как
SHA-256 (с привязкой к пользователю) в `auth_tokens` со сроком действия
(подтверждение — 24 ч, сброс — 1 ч); 5 неверных попыток — код сгорает.
События безопасности пишутся в `audit_logs`.

### Чаты

| тип | payload | ответ |
| --- | --- | --- |
| `chat.list` | `limit?` | `chats[]` с `members[]` и последним сообщением |
| `chat.open` | `contact` (email или id), `title?` | `chat`, `created` |
| `chat.history` | `chat_id`, `before_id?`, `limit?` | `messages[]`, `chat` |
| `chat.members` | `chat_id` | `members[]` с флагом `online` |
| `chat.read` | `chat_id` | `read` |
| `chat.send` | `chat_id`, `body` (≤ 8000 симв.), `kind?`, `payload?` | сообщение; от клиента допустим только `kind: "text"` (`agent_action`/`agent_reply`/`system` ставит сервер) — иначе `bad_request`; `payload` — объект ≤ 4 КБ без служебных полей `origin`, `proposal`, `confirmed`, `tool` |
| `users.search` | `query` (≥ 2 симв.), `limit?` | `users[]`: только `id`, `display_name`, `avatar_url`, `online` (без email и `last_seen_at`); поиск по подстроке имени, по email — только точное совпадение (запрос с `@`) |

События: `chat.message` (всем участникам, включая другие устройства отправителя),
`chat.created` (приглашённому).

### Selfvoy

| тип | payload | ответ |
| --- | --- | --- |
| `agent.ask` | `message`, `chat_id?`, `peers[]?`, `execute?` | `reply`, `intent`, `confidence`, `plan`, `actions[]`, `results[]`, `memory_saved`, `a2a`; `chat_id` чужого чата → `forbidden`; `peers[]` учитываются только люди, с которыми есть общий чат, и их контекст урезается по их `prefs.privacy` (как в A2A) |
| `agent.negotiate` | `target` (email/имя), `topic?`, `duration_minutes?`, `window_hours?` | `accepted`, `slot`, `place`, `counter_slots[]`, `message` |
| `agent.status` | — | `available`, `ai_service` |

Цепочка `agent.ask`:

1. `AgentManager` собирает контекст (память, настройки, расписание, история чата);
2. `POST /v1/agent/run` в Python AI Service с `execute: false` — AI только планирует;
3. C++ `ToolManager` выполняет вернувшиеся действия (почта, заметки, бронь…);
4. `MemoryManager` сохраняет `memory_updates` в `user_memory`;
5. если есть `a2a` — сервер находит пользователя-собеседника, вызывает
   `POST /v1/agent/negotiate` и публикует итог в чат сообщением с `kind: agent_reply`.

### Речь (STT)

| тип | payload | ответ |
| --- | --- | --- |
| `speech.transcribe` | `audio` (base64), `language?` (`ru`/`en`/`auto`, по умолч. `auto`), `format?` | `text`, `language`, `provider` |

Гибридная схема: на платформах с системным распознаванием (iOS Speech
Framework) клиент распознаёт на устройстве; на десктопе клиент пишет микрофон
(16 кГц, моно, 16 бит → WAV), кодирует в base64 и шлёт `speech.transcribe`.
C++-сервер форвардит аудио в Python AI Service `POST /v1/speech/transcribe`
(Whisper-совместимый). Провайдер STT выбирается на AI-сервисе: без
`SELFVOY_STT_URL`/`SELFVOY_STT_API_KEY` работает детерминированный `mock`
(для разработки), иначе — реальный Whisper-бэкенд. Пустое аудио → `bad_request`,
превышение `SELFVOY_STT_MAX_BYTES` → ошибка, недоступность бэкенда → `upstream_error`.

Озвучка (TTS) выполняется платформенно на клиенте (Qt TextToSpeech /
AVSpeechSynthesizer), отдельного сетевого метода не требует.

### Память и настройки

| тип | payload | ответ |
| --- | --- | --- |
| `memory.list` | `query?`, `limit?` | `entries[]` (сначала релевантные запросу) |
| `memory.add` | `text`/`entries[]`, `kind?` | `saved` |
| `memory.extract` | `message`, `commit?` | `entries[]`, `source` (`ai` или `local`) |
| `prefs.get` | — | настройки |
| `prefs.set` | любые поля `user_preferences` (в корне payload; `npd_receipts` — bool, включение — Pro, F10) | обновлённые настройки |

Онбординг-опрос после регистрации: клиенты показывают анкету (день
рождения, аллергии, диета, город, транспорт, бюджет), пока
`prefs.onboarded != true`. Флаг выставляет **сервер** в `prefs.set`, как
только в payload пришло хотя бы одно поле анкеты (`birthday`, `allergies`,
`diet`, `transport`, `city`, `budget_limit`, `preferred_hours`,
`work_hours`); клиент может передать `onboarded` явно. Анкета — обычные
поля настроек, она сразу попадает в контекст AI (`preferences`).

Поля настроек: `diet[]`, `transport`, `preferred_hours[]`, `work_hours[]`,
`birthday` (`YYYY-MM-DD`), `allergies[]`, `onboarded` (bool),
`budget_limit`, `city`, `lat`, `lon`, `theme`, `notifications{}`, `privacy{}`.

`privacy` — что Selfvoy раскрывает чужому Selfvoy в переговорах (`agent.negotiate`,
A2A из `agent.ask`); все флаги по умолчанию `true`, `prefs.get` всегда отдаёт
все три, `prefs.set` принимает любую их часть (неизвестный ключ или не bool →
`bad_request`):

| флаг | что делает сервер |
| --- | --- |
| `share_free_slots_only` | в контекст респондента для AI идут только занятость и настройки, нужные для времени и места: факты памяти, `birthday`, `allergies` вырезаются |
| `hide_event_titles` | названия событий календаря респондента заменяются на «занято» |
| `allow_place_choice` | `false` у любой из сторон — место не предлагается (`place` пустой, в `message` «Место выберете сами») |

### Инструменты

| тип | payload | ответ |
| --- | --- | --- |
| `tool.list` | — | `tools[]`, `mode` |
| `tool.run` | `tool`, `args{}`, `chat_id?` | результат инструмента; проходит тот же барьер `tool_permissions`, что и действия Selfvoy: `deny` → `forbidden`, `ask` (по умолчанию для опасных) → `{requires_confirmation: true, confirmation_id, summary, tool, status: "pending"}` без исполнения (дальше `confirmation.approve`/`deny`) |

Инструменты: `send_message`, `create_note`, `create_reminder`, `send_email`,
`find_cafe`, `book_table`, `check_calendar`, `suggest_time`. В режиме
`SELFVOY_TOOLS_MODE=sandbox` внешние API заменяются детерминированными ответами
(тот же контракт, что в `ai/selfvoy_ai/tools.py`). `tool.list` возвращает для
каждого инструмента флаг `dangerous` и `default_mode`.

### Разрешения и подтверждения (этап 8)

Ядро безопасности v3: **LLM лишь формирует намерение, исполняет сервер и только
после проверки прав.** Каждое действие Selfvoy проходит барьер:

1. сервер вычисляет эффективный режим инструмента — явное разрешение
   пользователя из `tool_permissions` или режим по умолчанию
   (опасные `send_message`/`send_email`/`book_table` → `ask`, остальные → `allow`);
2. `allow` — исполнить сразу; `deny` — отклонить; `ask` — **не исполнять**, а
   создать отложенное действие (`pending_actions`) и вернуть в `results[]`
   элемент с `requires_confirmation: true` и `confirmation_id`;
3. пользователь подтверждает (`confirmation.approve`) или отклоняет
   (`confirmation.deny`); только после подтверждения сервер исполняет инструмент.

Элемент `results[]` для действия, требующего подтверждения:
`{ "tool": "send_email", "mode": "ask", "requires_confirmation": true,
"confirmation_id": 12, "summary": "Отправить письмо на x@y.z", "ok": false }`.
Отклонённое по `deny`: `{ "tool": "...", "mode": "deny", "denied": true, "ok": false }`.

| тип | payload | ответ |
| --- | --- | --- |
| `permissions.list` | — | `tools[]`: `tool`, `description`, `dangerous`, `default_mode`, `mode` (эффективный) |
| `permissions.set` | `tool`, `mode` (`allow`/`ask`/`deny`) | `tool`, `mode`; неизвестный инструмент или режим → `bad_request` |
| `confirmation.list` | `status?` (по умолч. `pending`), `limit?` | `actions[]`: `id`, `tool`, `args{}`, `summary`, `status`, `result{}`, `created_at` |
| `confirmation.approve` | `id` | `id`, `status` (`executed`/`failed`), `result{}`; чужое/несуществующее → `not_found`, уже обработанное (в т.ч. параллельный второй approve) → `bad_request` |
| `confirmation.deny` | `id` | `id`, `status` (`denied`) |

Разрешения хранятся в `tool_permissions(user_id, tool, mode)`; отложенные
действия — в `pending_actions(id, user_id, chat_id, tool, args, summary, status,
result, …)` со статусами `pending`/`approved`/`denied`/`executed`/`failed`/
`expired`. Успешное подтверждённое действие с `chat_id` публикуется в чат
сообщением `kind: agent_action` (`origin: selfvoy-agent`, `confirmed: true`).
Изменения разрешений и подтверждения пишутся в `audit_logs`
(`tool_permission`, `action_approved`, `action_denied`).

### Задачи и напоминания (этап 8)

Задачи хранятся в `tasks(id, user_id, chat_id, title, notes, status, priority,
due_at, remind_at, reminded_at, …)`; статусы `pending`/`done`/`cancelled`.
Напоминание — задача с `remind_at`: фоновый планировщик сервера
(`SELFVOY_SCHEDULER_INTERVAL_MS`, по умолчанию 30 с) находит задачи с наступившим
`remind_at`, помечает их отправленными (`reminded_at`, одноразово) и шлёт
владельцу push-событие `task.due`. Инструмент `create_reminder` тоже создаёт
задачу (возвращает `task_id`).

| тип | payload | ответ |
| --- | --- | --- |
| `tasks.list` | `status?`, `limit?` | `tasks[]` |
| `tasks.create` | `title`, `notes?`, `due_at?`, `remind_at?`, `priority?`, `chat_id?` | задача |
| `tasks.complete` | `id` | задача со статусом `done` |
| `tasks.cancel` | `id` | задача со статусом `cancelled` |
| `tasks.reopen` | `id` | задача со статусом `pending` |
| `tasks.delete` | `id` | `deleted`; чужая/несуществующая → `not_found` |
| `tasks.due` | — | `sent` — сколько напоминаний **вызывающего пользователя** отправлено (ручной проход планировщика только по своим задачам) |

Событие: `task.due` (владельцу задачи, когда наступил срок напоминания).

### Интеграции: Google OAuth 2.0 + PKCE (этап 9)

Провайдеры: `google_calendar` (scope `calendar.events.readonly`) и
`google_gmail` (scope `gmail.send`). Поток подключения: `integrations.begin` →
пользователь открывает `authorize_url` и разрешает доступ на consent-экране
Google → Google редиректит на `redirect_uri` с `code` + `state` → клиент
шлёт `integrations.callback`. Сервер обменивает код на токены (PKCE
`code_verifier`, метод S256), шифрует их AES-256-GCM и сохраняет в
`integration_connections`; `state`/`verifier` одноразовые
(`integration_oauth_states`, TTL `SELFVOY_OAUTH_STATE_TTL`, по умолчанию 600 с).
**Токены клиенту не отдаются никогда.**

| тип | payload | ответ |
| --- | --- | --- |
| `integrations.list` | — | `providers[]` (`provider`, `name`, `description`, `scope`), `connections[]` (`id`, `provider`, `account`, `scope`, `status`, `last_error?`, `created_at`, `last_used_at?`) |
| `integrations.begin` | `provider`, `redirect_uri` | `provider`, `authorize_url`, `state`, `expires_in`; неизвестный провайдер → `bad_request`, OAuth не настроен → `not_configured` |
| `integrations.callback` | `provider`, `code`, `state` | подключение (без токенов); недействительный/повторный/истёкший `state` → `bad_request`, отказ провайдера → `oauth_error`, эндаунт недоступен → `upstream_error` |
| `integrations.revoke` | `id` | `id`, `status=revoked`; чужое/несуществующее → `not_found`; провайдер недоступен → `upstream_error` (запись сохраняется для повтора) |
| `integrations.sync` | `id` | календарь: `events`, `facts` (события сохраняются в память, kind `schedule.google`); Gmail: `profile_email` |

Access-токен автоматически обновляется по `refresh_token` (при HTTP 401 или
перед истечением). Инструменты используют подключения прозрачно: `send_email`
отправляет письмо через Gmail API (`messages.send`, RFC 2822 в base64url),
если подключён `google_gmail`, иначе — прежний почтовый путь; `check_calendar`
возвращает реальные события (`source=google_calendar`), иначе память
(`source=memory`).

Переменные окружения: `SELFVOY_GOOGLE_CLIENT_ID`, `SELFVOY_GOOGLE_CLIENT_SECRET`,
`SELFVOY_GOOGLE_AUTH_URL`, `SELFVOY_GOOGLE_TOKEN_URL`, `SELFVOY_GOOGLE_REVOKE_URL`,
`SELFVOY_GOOGLE_CALENDAR_URL`, `SELFVOY_GOOGLE_GMAIL_URL`, `SELFVOY_OAUTH_STATE_TTL`.
По умолчанию URL — реальные эндпоинты Google (прод — за TLS-терминирующим
прокси); e2e подменяет их локальным mock-сервером (`SELFVOY_E2E_GOOGLE_MOCK=1`).

### Уведомления и push (этап 13)

Единая «входящая» уведомлений: каждое уведомление сохраняется в таблице
`notifications` и доставляется живым сессиям событием `notification.new`.
Виды (`kind`): `task.due` (напоминание), `confirmation.requested` (опасная
операция ждёт решения), `a2a.proposal` (другой агент начал переговоры),
`login.new` (новый вход), `twofactor.enabled` / `twofactor.disabled`
(изменение 2FA), `invite.accepted` / `invite.declined` (гость ответил на
приглашение по ссылке), `booking.created` / `booking.rescheduled` /
`booking.cancelled` (гость записался, перенёс или отменил встречу на
странице записи; `payload.cancelled_by: "system"` — запись отменена, потому что
оплата не поступила), `booking.confirmed` (гость нажал «Приду»), `booking.payment`
(оплата записи: `{booking_id, page_id, status, amount, currency, guest_name}`,
`status` — `claimed` гость отметил «Я оплатил(а)», `paid` оплата через ЮKassa
прошла, `refunded` деньги возвращены через ЮKassa, `refund_due` — верните оплату
гостю вручную), `package.purchased` (гость купил пакет при записи:
`{package_id, page_id, guest_name}`), `package.payment` (оплата пакета:
`{package_id, page_id, guest_name, status, amount, currency}`, `status` —
`claimed` / `paid` / `refunded` / `refund_due` как у записи, `expired` — оплата не
поступила, покупка отменена), `package.low` (у пакета осталось 1 или 0 занятий:
`{package_id, page_id, guest_name, left}`), `waitlist.joined` / `waitlist.booked`
(лист ожидания, F8), `tax.receipt` / `tax.refund` (чек «Мой налог», F10),
`referral.joined`
(приглашённый по реферальной ссылке подтвердил email), `referral.reward`
(приглашённому начислен месяц Pro). Push-доставка — через устройства в `push_devices`
(платформы `apns` | `fcm` | `webhook` | `dev`); платформенный конверт
уходит во внешний шлюз (`SELFVOY_PUSH_DRIVER=webhook`,
`SELFVOY_PUSH_WEBHOOK_URL`), который подписывает ключи провайдера (JWT для
APNs, OAuth2 сервисного аккаунта для FCM) и форвардит запрос — сам
C++-сервер TLS/HTTP2 не делает и ключи провайдеров не хранит.
Тихие часы и отключённые виды читаются из настроек пользователя:
`prefs.notifications = {"quiet_hours": {"start":"22:00","end":"08:00"},
"muted_kinds": ["login.new"]}` (время UTC; гасят только push и Telegram, in-app
доставляется всегда). `prefs.notifications.telegram: false` — не дублировать
уведомления в Telegram (см. «Telegram-бот»).

| тип | payload | ответ |
| --- | --- | --- |
| `notifications.list` | `unread?` (bool), `limit?` (≤200) | `notifications[]` (`id`, `kind`, `title`, `body`, `payload`, `read`, `created_at`), `unread` (счётчик) |
| `notifications.read` | `id?` (0/нет — все) | `unread`; чужое/несуществующее `id` → `not_found` |
| `devices.push.register` | `platform`, `token` | `id`, `platform`; повтор тем же токеном — upsert; плохие входные данные → `bad_request` |
| `devices.push.list` | — | `devices[]` (`id`, `platform`, `enabled`, `created_at`, `last_used_at?`); **токен не отдаётся** |
| `devices.push.revoke` | `id` | `id`, `revoked`; чужое/несуществующее → `not_found` |

Событие: `notification.new` (владельцу; payload — запись уведомления).
Push-конверт для шлюза: `{platform, token, notification_id, kind, …}`, где
платформенная секция зависит от `platform`:

- `apns` (и устройства без специальной платформы):
  `apns: {url, headers: {apns-topic, apns-push-type}, payload: {aps:
  {alert, badge, sound}, kind, data}}`;
- `fcm` (Android, этап 11): `fcm: {url, message: {token, notification:
  {title, body}, android: {priority, notification: {channel_id: "selfvoy",
  sound}}, data: {kind, notification_id, data}}}` — тело готово к
  `POST {fcm.url}` (FCM HTTP v1) с заголовком `Authorization: Bearer
  <OAuth2-токен сервисного аккаунта>`; значения `data` — строки
  (требование FCM), полезная нагрузка уведомления сериализуется в
  `data.data`.

Переменные окружения: `SELFVOY_PUSH_DRIVER` (`dev`|`webhook`),
`SELFVOY_PUSH_WEBHOOK_URL`, `SELFVOY_APNS_TOPIC` (`app.selfvoy`), `SELFVOY_APNS_URL`
(`https://api.push.apple.com`), `SELFVOY_FCM_URL`
(`https://fcm.googleapis.com/v1/projects/selfvoy/messages:send` — замените
`selfvoy` на id вашего проекта Firebase).

### Приглашения по ссылке (встреча с человеком без Selfvoy)

Владелец договаривается о встрече с человеком, у которого нет Selfvoy: сервер
подбирает свободные слоты владельца (AI-сервис, `POST /v1/agent/slots` — только
рабочие часы в часовом поясе владельца, не раньше чем через час, до 2 слотов в
день, не больше 6) и выдаёт ссылку. Гость открывает её в веб-клиенте
(`<SELFVOY_PUBLIC_URL>/#/i/<токен>`) без регистрации, выбирает время или
отказывается. Сервер не доверяет ответу AI: проверяет длительность, окно,
будущее время, занятость и пересечения. Токен — 32 случайных байта
(base64url, 43 символа); в БД только его sha256, поэтому ссылка показывается
владельцу **один раз** — в `invite.list` её нет.

Методы владельца (нужен вход):

| тип | payload | ответ |
| --- | --- | --- |
| `invite.create` | `{topic (1–120 символов, одна строка), duration_minutes? (15–240, 60), window_hours? (1–336, 168), note? (≤ 500), guest_name? (≤ 80)}` | `{invite, token, url}` — `url` пуст, если не задан `SELFVOY_PUBLIC_URL` (клиент строит ссылку сам) |
| `invite.list` | `{limit? (1–100, 50)}` | `{invites: [invite]}` — новые сверху |
| `invite.cancel` | `{invite_id}` | `{invite}` — только своё и открытое |

`invite` (вид владельца): `{id, topic, note, duration_minutes, status, slots:
[{start, end}], guest_name, guest_email, guest_comment, chosen: {start, end} | {},
expires_at, created_at, responded_at}`. `status`: `open` | `accepted` |
`declined` | `cancelled` | `expired` (открытое, у которого прошло начало
последнего слота; в БД не хранится). Ошибки `invite.create`: `bad_request`;
`conflict` «нет свободных окон в выбранный период»; `upstream_error` — AI-сервис
недоступен; `forbidden` — больше 20 открытых приглашений.

Методы гостя (публичные, без входа; доступ — по токену из ссылки):

| тип | payload | ответ |
| --- | --- | --- |
| `invite.get` | `{token}` | `{invite}` — вид гостя |
| `invite.respond` | `{token, action: "accept" \| "decline", start? (для accept — start одного из слотов), guest_name (1–80), guest_email? (≤ 254), comment? (≤ 500)}` | `{invite}` — вид гостя |

Вид гостя: `{topic, note, duration_minutes, owner_name, owner_timezone, status,
slots, guest_name, chosen, expires_at, ref}` (`ref` — реферальный код владельца) — без email владельца и данных других
гостей; `slots` у открытого — только будущие и ещё свободные у владельца (может
быть пустым). Ошибки: `not_found` — неизвестный токен; `conflict` — приглашение
уже закрыто, срок истёк или время уже занято; `bad_request` — слот не из списка,
пустое имя, неверный email; `forbidden` — больше 30 `invite.get` или 10
`invite.respond` в минуту с одного IP.

`accept` атомарно закрывает приглашение (второй ответ — `conflict`) и
добавляет владельцу: запись расписания «Встреча: <тема> — <гость>
(<start>/<end>)» (интервал занимает время в переговорах и других
приглашениях), задачу с `due_at = start` и `remind_at = start − 15 мин`,
уведомление `invite.accepted` (`payload: {invite_id, start, end, guest_name,
task_id}`). `decline` — уведомление `invite.declined` (`payload: {invite_id,
guest_name, comment}`).

### Страницы записи (постоянная ссылка для людей без Selfvoy)

Владелец заводит постоянную публичную ссылку `<SELFVOY_PUBLIC_URL>/#/b/<slug>`
(`slug` — 10 символов `[a-z0-9]`). Гость без регистрации видит свободное время
владельца и записывается сам — на одну встречу или на еженедельную серию.
Свободное время подбирает AI-сервис (`POST /v1/agent/slots` с `per_day`,
`lead_minutes`, `weekends`; только рабочие часы в поясе владельца), ответ
кэшируется на минуту. Сервер не доверяет AI: проверяет длительность, окно,
пересечения, а при каждом показе и записи — запас до начала, занятость
владельца (расписание, другие записи, встречи из приглашений) и лимит записей
в местный день владельца. Встречи записей сами занимают время владельца — в
переговорах, приглашениях и на других страницах.

Методы владельца (нужен вход):

| тип | payload | ответ |
| --- | --- | --- |
| `booking.pageCreate` | `{title (1–120, одна строка), description? (≤ 1000), duration_minutes? (15–240, 30), days_ahead? (1–60, 14), max_per_day? (1–16, 4), notice_hours? (0–168, 2), max_series? (1–12, 1), weekends? (bool, false), guest_reminders? (bool, true), hide_branding? (bool, false), …поля цены и политики отмены (F4, ниже), packages? (предложения пакетов, F5, ниже), waitlist? / waitlist_hold_minutes? / rebook_after_days? (лист ожидания и повторный приём, F8, ниже)}` | `{page}`; больше 10 страниц → `forbidden`; `hide_branding: true`, `prepay` ≠ `none`, непустой `packages`, `waitlist: true` или `rebook_after_days` > 0 без Pro при `SELFVOY_PLANS_ENFORCED=1` → `pro_required` |
| `booking.pageList` | — | `{pages: [page]}` — новые сверху, без удалённых |
| `booking.pageUpdate` | `{page_id, …любые поля pageCreate, status? (active \| paused)}` | `{page}` — меняются только переданные поля (`packages` — целым массивом); включить `hide_branding`, предоплату (`prepay` из `none`) или пакеты (непустой `packages` у страницы без пакетов) без Pro (при `SELFVOY_PLANS_ENFORCED=1`) → `pro_required`, выключить и править уже включённое можно всегда |
| `booking.pageDelete` | `{page_id}` | `{deleted: true}` — ссылка перестаёт работать, сделанные записи остаются |
| `booking.list` | `{page_id? (0 — все), active_only?, limit? (1–200, 50)}` | `{bookings: [booking]}` — новые сверху |
| `booking.ownerCancel` | `{booking_id, occurrence? (start одной встречи)}` | `{booking}`; без `occurrence` — все предстоящие встречи; оплаченная запись, отменённая целиком, — возврат (см. «Цена, предоплата и политика отмены») |
| `booking.paymentSet` | `{booking_id, status: paid \| waived \| refunded}` | `{booking}` — отметка оплаты владельцем (`paid_at` / `refunded_at` = сейчас); у записи нет оплаты → `bad_request`; `paid` — гостю письмо «Оплата получена» |

`page`: `{id, slug, url, title, description, duration_minutes, days_ahead,
max_per_day, notice_hours, max_series, weekends, status (active | paused),
guest_reminders, hide_branding, price, currency, prepay, deposit, payment_url,
payment_note, pay_within_minutes, cancel_hours, late_cancel, policy_text, packages,
waitlist, waitlist_hold_minutes, rebook_after_days (F8), upcoming, created_at, updated_at}`; `url` пуст
без `SELFVOY_PUBLIC_URL`. `guest_reminders` — напоминать гостю о встрече
(бесплатно), `hide_branding` — скрыть подпись «Запись через Selfvoy» (Pro).
`booking` (вид владельца): `{id, page_id, page_title, guest_name, guest_email,
guest_comment, cc_email (второй получатель, "" — нет; F5), package_id (пакет записи,
0 — без пакета; F5), price, currency, status (active | cancelled), cancelled_by ("" | guest | owner),
reschedules, occurrences: [{start, end, local, status (active | cancelled),
confirmed_at}], reminders: [{start, kind (24h | 2h), due_at, sent_at}],
payment (объект оплаты целиком или null), rebook_due_at, rebook_sent_at (F8), created_at, updated_at}`;
`price` — цена услуги (одной встречи) на момент записи, в минимальных единицах
`currency` (0 — по абонементу или бесплатно): снимок цены страницы при
`booking.book`, последующая смена цены страницы (`booking.pageUpdate`), перенос и
отмена его не меняют; запись по пакету (`package_id` / `package_token`) — 0.
`payment.amount` — сколько нужно внести / внесено онлайн (депозит или полная
предоплата), это не цена услуги. Старый сервер `price` у записи не отдаёт —
клиент тогда берёт цену страницы.
`cancelled_by` у отменённой записи — `guest`, `owner` или `system` (оплата не
поступила к сроку); `confirmed_at` — когда гость нажал «Приду» ("" — не
подтверждал), `reminders` — запланированные и отправленные напоминания гостю
(`sent_at` "" — ещё не отправлено).

Методы гостя (публичные, без входа). Соединение без входа сервер закрывает
через `SELFVOY_AUTH_TIMEOUT` (30 с), но если по нему успешно вызывались гостевые
методы `invite.*` / `booking.*` — через 10 минут после последнего такого вызова:
гость успевает выбрать время, а ошибочные запросы срок не продлевают.

| тип | payload | ответ |
| --- | --- | --- |
| `booking.get` | `{slug}` | `{page}` — вид гостя |
| `booking.book` | `{slug, start (start свободного слота), guest_name (1–80), guest_email? (≤ 254), cc_email? (≤ 254, ≠ guest_email; F5), comment? (≤ 500), count? (1–max_series, 1), package_id? (id предложения пакета — купить пакет; F5), package_token? (токен ссылки пакета — запись по пакету; F5)}` | `{booking, token, url, telegram_url}` — токен ссылки управления (43 символа base64url) показывается **один раз**; `url` — `<SELFVOY_PUBLIC_URL>/#/m/<токен>` или пусто; `telegram_url` — ссылка на бота для напоминаний (`https://t.me/<бот>?start=g_…`) или "" (бот не настроен). С `package_id` дополнительно `package_token` (показывается **один раз**), `package_url` (`<SELFVOY_PUBLIC_URL>/#/p/<токен>` или "") и `package` (вид гостя); с `package_token` — `package`. С пакетом `guest_name` можно не передавать (берётся из пакета) |
| `booking.manage` | `{token}` | `{booking}` — вид гостя по ссылке управления |
| `booking.reschedule` | `{token, start, occurrence? (start переносимой встречи; по умолчанию ближайшая)}` | `{booking}`; не больше 3 переносов на запись |
| `booking.cancel` | `{token, occurrence?, reason? (≤ 500)}` | `{booking}`; без `occurrence` — все предстоящие встречи |
| `booking.resendLink` | `{slug, email}` | `{sent: true}` — всегда одинаково (нельзя узнать, есть ли запись); если на странице есть активная запись с этим email, ссылка управления перевыпускается (прежняя перестаёт работать) и уходит письмом |
| `booking.confirm` | `{token, occurrence? (start встречи; по умолчанию ближайшая предстоящая активная)}` | `{booking}` (вид гостя, `slots` пуст) — у встречи ставится `confirmed_at`; повтор ничего не меняет и не уведомляет владельца второй раз; лимит — как у переноса/отмены |
| `booking.paymentClaim` | `{token}` | `{booking}` (вид гостя, `slots` пуст) — «Я оплатил(а)» для оплаты по ссылке: `pending` → `claimed`, владельцу — `booking.payment` (`claimed`); повтор и уже `paid`/`waived` — без изменений; оплата через ЮKassa → `bad_request`; нет оплаты → `bad_request`; запись отменена (оплата `expired`) → `conflict`; лимит — как у переноса/отмены |

Письма гостю (если при записи указан email; драйвер почты — как у кодов
подтверждения, `SELFVOY_MAIL_DRIVER`; копия каждого — второму получателю
`cc_email`, F5): подтверждение записи со ссылкой управления
(и строкой «Напоминания в Telegram: <url>», если бот настроен),
перенос, отмена гостем и отмена владельцем (со ссылкой «Выбрать другое время»),
новая ссылка по `booking.resendLink`. Те же извещения уходят в Telegram-чаты,
подключённые к записи (простой текст; ссылка управления — url-кнопкой, если адрес
https); лимит — 10 в час на адрес (у `cc_email` — свой) и на чат, 200 в час на владельца. Время — местное время владельца с
поясом. В письмах только текст сервера и владельца (название страницы, имя
владельца): имя и комментарий гостя туда не попадают, на один адрес — не больше
10 писем в час.

Вид страницы для гостя: `{slug, title, description, duration_minutes,
max_series, owner_name, owner_timezone, status, slots: [{start, end, day}],
slots_unavailable, ref, branding, price, currency, prepay, deposit, payment_note,
payment_url, pay_within_minutes, policy: {cancel_hours, late_cancel, text},
packages: [{id, title, sessions, price, valid_days}], waitlist, waitlist_hold_minutes,
rebook_after_days (F8)}` —
без email владельца и чужих записей; `day` —
местная дата владельца; `slots_unavailable: true` — AI-сервис недоступен
(страница видна, время не подобрано); `ref` — реферальный код владельца для
ссылки «Заведите свой Selfvoy». Вид записи для гостя: `{page_title, page_slug
("" — запись на странице закрыта), owner_name, owner_timezone,
duration_minutes, guest_name, cc_email (замаскирован: `m***@example.com`; "" — нет),
by_package (запись по пакету), price, currency (цена встречи на момент записи — как в
виде владельца; 0 — по абонементу или бесплатно), status, cancelled_by, occurrences, can_cancel,
can_reschedule, can_change, reschedules_left, slots (свободное время для
переноса ближайшей встречи), slots_unavailable, ref, branding, telegram_url,
telegram_linked, payment (без `provider_id`; null — оплаты нет), receipt_url (ссылка на
чек «Мой налог», "" — нет; F10), late, policy, payment_note}`, у каждой встречи в `occurrences` — `confirmed_at` ("" — не
подтверждена); `telegram_url` — ссылка подписки на напоминания в Telegram
("" — бот не настроен или запись отменена; после `booking.resendLink` прежняя
ссылка перестаёт работать), `telegram_linked` — к записи уже подключён чат. `can_cancel` — есть
предстоящая встреча (отменить можно и на паузе, и после удаления страницы);
`can_reschedule` — к тому же страница открыта и переносы не исчерпаны;
`can_change` — устаревший синоним `can_reschedule`. `late` — до ближайшей встречи
меньше `cancel_hours` часов; при `late_cancel: block` тогда `can_cancel` и
`can_reschedule` — `false`.

Подпись (F3): `branding: {show, text: "Запись через Selfvoy", url}`, `url` —
`<SELFVOY_PUBLIC_URL>/#/signup?ref=<код владельца>&src=booking` ("" без
`SELFVOY_PUBLIC_URL`). `show: false` — только пока владелец скрыл подпись
(`hide_branding`) и у него действует Pro (или `SELFVOY_PLANS_ENFORCED=0`):
Pro закончился — подпись снова показывается. При `show` и непустом `url` в
письма гостю последней строкой и в сообщения бота гостю добавляется «Запись
через Selfvoy — заведите свой бесплатно: <url>». Поле `ref` оставлено для
совместимости.

Напоминания гостю (F1, если у страницы `guest_reminders`): при записи и переносе
на каждую активную встречу планируются `24h` (за сутки; если до начала ≥ 25 ч)
и `2h` (за 2 часа; если до начала ≥ 2 ч 30 мин); отмена или перенос встречи
удаляет её неотправленные напоминания. Планировщик сервера отправляет наступившие
(атомарный захват: на PostgreSQL `FOR UPDATE SKIP LOCKED` — каждое уходит один
раз даже при нескольких экземплярах) письмом и в Telegram-чаты записи с теми же
лимитами, что у извещений. Не отправляются, если запись или встреча уже не
активна, встреча началась, у страницы выключен `guest_reminders` (проверяется в
момент отправки) или «за сутки» опоздало до последних 2 часов. Тема — «Напоминание:
завтра в 15:00 — <название страницы>» («сегодня в …» или дата), время — местное
время владельца, в тексте — с поясом и имя владельца. Кнопка письма «Приду» —
`<SELFVOY_PUBLIC_URL>/#/m/<токен>?confirm=<start ISO>`, строкой — «Перенести или
отменить: <SELFVOY_PUBLIC_URL>/#/m/<токен>»; у старых записей (токен не сохранён)
или без `SELFVOY_PUBLIC_URL` вместо ссылок — «Перенести или отменить можно по
ссылке из письма о записи». В Telegram — кнопка «Приду» (callback
`cf:<booking_id>:<start_unix>`) и url-кнопка «Перенести или отменить» (только
https-ссылка). «Приду» по ссылке вызывает `booking.confirm`; владельцу —
уведомление `booking.confirmed` (`{booking_id, page_id, start, guest_name}`) —
один раз на встречу. Перенос встречи сбрасывает её `confirmed_at`.

Серия — та же встреча каждую неделю в том же **местном времени владельца**
(AI `POST /v1/agent/series` считает повторы в его поясе: 10:00 остаётся 10:00
после перехода на летнее/зимнее время; сервер проверяет первую встречу,
длительность и шаг «неделя ± 2 часа»). Каждая встреча должна быть свободна и не
превышать лимит своего дня, иначе `conflict` «в неделю … это время занято»;
без AI-сервиса серия не записывается (`upstream_error`). У каждой встречи в
`occurrences` есть `local` — местное время владельца `YYYY-MM-DDTHH:MM`. На каждую встречу владельцу создаётся задача с `due_at = start`
и `remind_at = start − 15 мин` (при переносе — пересоздаётся, при отмене —
удаляется) и уведомление `booking.created` (`{booking_id, page_id, start, end,
count, guest_name, package_id?}` — `package_id` у записи по пакету), `booking.rescheduled` (`{booking_id, page_id, from,
start, end, guest_name}`), `booking.cancelled` (`{booking_id, page_id,
cancelled, guest_name, reason}`). Запись без предстоящих встреч получает
`status: cancelled`.

Ошибки: `not_found` — неизвестный/удалённый slug или токен, чужая запись;
`conflict` — время заняли или его нет среди свободных (`booking.book` не
различает эти случаи, чтобы по ответам нельзя было узнать личное расписание
владельца), запись на паузе, запись уже отменена, переносы исчерпаны,
подтверждаемой встречи нет (прошла или отменена), до встречи меньше
`cancel_hours` при `late_cancel: block` («до встречи меньше N ч — свяжитесь со
специалистом»); `bad_request` — неверные поля (при переносе — и время не из
свободных); `upstream_error` — AI-сервис недоступен при записи/переносе, ЮKassa не
создала платёж; `forbidden` — лимиты по IP (30 `booking.get`/`booking.manage` в
минуту, 10 записей в минуту и 30 в час, 10 переносов/отмен в минуту; IPv6 — по
сети /64) и не больше 20 состоявшихся записей на страницу в час. Писем гостям —
до 10 в час на ящик (регистр, «+метка» и точки Gmail не дают обойти) и до 60 в
час на владельца.

### Цена, предоплата и политика отмены (F4)

Поля страницы записи (`booking.pageCreate` / `booking.pageUpdate`; в
`pageUpdate` меняются только переданные):

| поле | значение |
| --- | --- |
| `price` | цена в минимальных единицах валюты (копейки), целое 0–100 000 000; 0 — не показывать |
| `currency` | `RUB` (по умолчанию) \| `USD` \| `EUR` \| `KZT` \| `BYN` |
| `prepay` | `none` (по умолчанию) \| `full` (вся сумма: `price × число встреч`) \| `deposit` (депозит один раз на запись) |
| `deposit` | депозит в минимальных единицах; при `prepay: deposit` — от 1 до `price` |
| `payment_url` | ссылка оплаты владельца: только `https://`, ≤ 500 символов, без пробелов |
| `payment_note` | реквизиты / СБП — одна строка, ≤ 300 символов |
| `pay_within_minutes` | сколько держится время до оплаты: 15–1440, 60 |
| `cancel_hours` | отмена с возвратом — не позже чем за N часов: 0–168, 0 (отменять можно всегда) |
| `late_cancel` | `allow` (по умолчанию: поздняя отмена разрешена, без возврата) \| `block` (позже срока гость не отменяет и не переносит) |
| `policy_text` | текст политики отмены для гостя, ≤ 500 символов |

`prepay` ≠ `none` требует `price > 0` и способ оплаты: `payment_url`,
`payment_note` или подключённую ЮKassa (иначе `bad_request`), а при
`SELFVOY_PLANS_ENFORCED=1` — действующий Pro для включения (`pro_required`; уже
включённую предоплату можно править и после окончания Pro). Цена без
предоплаты и политика отмены — бесплатно. Способ оплаты проверяется при
создании страницы и при правке полей оплаты (`prepay`, `price`, `deposit`,
`payment_url`, `payment_note`). Цена страницы попадает и в пост «свободные
окна» (`booking.slotsPost`).

Подключение ЮKassa владельцем (свой магазин — деньги гостей идут ему напрямую,
Selfvoy их не касается):

| тип | payload | ответ |
| --- | --- | --- |
| `payments.status` | — | `{connected, provider ("yookassa" \| ""), shop_id, connected_at}` |
| `payments.connect` | `{provider: "yookassa", shop_id (цифры, ≤ 20), secret_key (секретный ключ магазина live_…/test_…)}` | `{connected: true, provider, shop_id, connected_at}`; ключи проверяются запросом `GET /me` (Basic `shop_id:secret_key`): неверные ключи или ключ другого магазина → `bad_request`, ЮKassa не отвечает → `upstream_error`; без Pro при `SELFVOY_PLANS_ENFORCED=1` → `pro_required`; без `SELFVOY_PUBLIC_URL` на сервере → `bad_request` (гостю некуда вернуться после оплаты); 10 попыток в час |
| `payments.disconnect` | — | `{connected: false}` |

Секретный ключ хранится зашифрованным (AES-256-GCM, ключ — как у TOTP-секретов,
AAD `selfvoy-payment-secret`), наружу и в журнал не попадает. Адрес API —
`SELFVOY_YOOKASSA_API_URL` (по умолчанию `https://api.yookassa.ru/v3`). Чеки
54-ФЗ (объект `receipt`) сервер не отправляет: самозанятый формирует чек в «Мой
налог», ИП/организация — настройками магазина в личном кабинете ЮKassa.

Оплата записи — объект `payment` (`bookings.payment`; тот же формат у пакетов F5):
`{status, amount, currency, kind (full | deposit | package), method (link |
yookassa), provider_id (id платежа ЮKassa; гостю не отдаётся), action_url
(куда отправить гостя платить; "" — только реквизиты), due_at, paid_at,
refunded_at}` (+ поля чека «Мой налог», F10: `npd`, `receipt_task_id`, `receipt_url`,
`receipt_at`, `receipt_refund_task_id`); `status`: `pending` (ждём оплату) → `paid` / `claimed` (гость
сказал, что оплатил) / `expired` (не оплачено к сроку или запись отменена до
оплаты); владелец может поставить `paid`, `waived` (без оплаты) или `refunded`.

- `booking.book` на странице с предоплатой создаёт оплату `pending` на
  `price × count` (`full`) или `deposit`; `due_at` = сейчас + `pay_within_minutes`
  (не позже начала первой встречи). ЮKassa подключена — `POST /payments`
  (`Idempotence-Key: bk-<booking_id>`, `capture: true`, `confirmation: {type:
  redirect, return_url: <ссылка управления>?paid=1}`, `description` ≤ 128
  символов «<страница>, <дата>», `metadata.booking_id`), `action_url` —
  `confirmation_url` ЮKassa; платёж не создался → запись отменяется
  (`cancelled_by: system`), ответ `upstream_error`. Иначе способ `link`:
  `action_url` = `payment_url`, гость платит сам и нажимает «Я оплатил(а)»
  (`booking.paymentClaim`). Письмо о записи — с суммой, сроком, ссылкой оплаты
  и реквизитами. Если к моменту записи способа оплаты нет (ЮKassa отключена, а
  ссылки и реквизитов нет), запись создаётся без оплаты.
- Время держится до `due_at`. Не оплачено (`pending`; `claimed` не истекает) —
  планировщик отменяет запись (`cancelled_by: system`), оплата `expired`;
  гостю — письмо/Telegram «Selfvoy: оплата не поступила — запись отменена» со
  ссылкой «Выбрать другое время», владельцу — `booking.cancelled`
  (`cancelled_by: "system"`).
- ЮKassa: статус платежа проверяется в планировщике (не чаще раза в 30 с на
  платёж; к сроку — ещё раз) и при каждом `booking.manage`: `succeeded` →
  `paid` (владельцу `booking.payment`, гостю письмо «Оплата получена»),
  `canceled` → `expired` и отмена записи. ЮKassa не отвечает к сроку — запись
  ждёт до часа сверх срока. Гость заплатил уже после отмены — деньги
  возвращаются автоматически (при `booking.manage`).
- Возвраты (запись отменена целиком): владелец отменил оплаченную через ЮKassa
  запись — `POST /refunds` на всю сумму (`Idempotence-Key: rf-<booking_id>`),
  оплата `refunded`; оплаченную по ссылке (`paid`/`claimed`) — владельцу
  `booking.payment` `refund_due` «верните оплату вручную». Гость отменил не
  позже чем за `cancel_hours` — так же полный возврат (ЮKassa — автоматически,
  по ссылке — `refund_due` владельцу); позже — без возврата (депозит остаётся у
  специалиста, в письме об отмене — «оплата не возвращается»), а при
  `late_cancel: block` гость не может ни отменить, ни перенести встречу
  (`conflict` «до встречи меньше N ч — свяжитесь со специалистом»). Отмена одной
  встречи серии оплату не меняет. ЮKassa не приняла возврат — `refund_due`.

### Пакеты, абонементы и второй получатель (F5)

**Второй получатель.** `booking.book` принимает `cc_email` (≤ 254 символов,
отличается от `guest_email` без учёта регистра) — «копия напоминаний, например
родителю». Копия получает все извещения гостя: подтверждение со ссылкой
управления, напоминания, перенос, отмену, оплату (лимит писем — свой, 10 в час
на адрес). Вид владельца — `cc_email` целиком, вид гостя — замаскированный
(`m***@example.com`). Бесплатно.

**Предложения пакетов** — поле страницы `packages` (`booking.pageCreate` /
`booking.pageUpdate`, правка — целым массивом):
`[{id, title (1–80, одна строка), sessions (2–100), price (мин. единицы валюты
страницы, > 0, ≤ 100 000 000), valid_days? (7–365, 90)}]`, не больше 5.
Элементы с `id` сохраняют `id` (неизвестный или повторный `id` → `bad_request`),
без `id` — новые (id больше всех прежних). Непустой массив — Pro (при
`SELFVOY_PLANS_ENFORCED=1`, как скрытие подписи: включить без Pro нельзя, править
уже включённые — можно). Гость видит предложения в `booking.get` (`packages`).

**Покупка при записи** — `booking.book {…, package_id: <id предложения>}`
(`count` ≤ `sessions`): создаётся покупка-снимок предложения, запись
привязывается к ней и сразу списывает занятия. Оплата — как у записи (F4),
сумма = цена пакета, `payment.kind = package`, предоплата страницы не
применяется:
- ЮKassa владельца подключена — покупка `pending`, `POST /payments`
  (`Idempotence-Key: pk-<package_id>`, `return_url: <SELFVOY_PUBLIC_URL>/#/p/<токен>?paid=1`,
  `description` «<страница>: <пакет>», `metadata.package_id`); платёж не создался
  → покупка `cancelled`, запись отменена, `upstream_error`.
- Есть `payment_url` / `payment_note` — покупка `pending`, способ `link`;
  гость платит и нажимает «Я оплатил(а)» (`package.paymentClaim`).
- Способа оплаты нет — покупка сразу `active`, оплата `pending` без срока
  (`due_at: ""`), её отмечает владелец (`package.paymentSet`).
Время записи держится до `due_at` (= сейчас + `pay_within_minutes`, не позже
первой встречи). Оплата пришла (ЮKassa — планировщик раз в 30 с и при
`package.get`; `claimed`; `paymentSet paid|waived`) → покупка `active`,
`valid_until = момент активации + valid_days` (гостю письмо «Оплата пакета
получена», владельцу `package.payment` `paid`). Не оплачено к сроку (или ЮKassa
отменила платёж) → покупка `cancelled`, оплата `expired`, записи пакета
отменяются системой (`cancelled_by: system`, владельцу `booking.cancelled` и
`package.payment` `expired`, гостю «Оплата пакета не поступила» со ссылкой
страницы). Гость заплатил после отмены — автоматический возврат (`pr-<id>`, при
`package.get`). Письмо о записи — со строками пакета: занятия, срок, ссылка пакета,
сумма и способ оплаты. Владельцу — `package.purchased`.

**Запись по пакету** — `booking.book {slug, start, package_token, count?}`:
пакет этой страницы (чужая страница → `bad_request`), `active`, не истёк, остаток
≥ `count`, каждая встреча раньше `valid_until`; иначе `conflict` («абонемент ещё не
оплачен», «абонемент отменён», «срок действия абонемента истёк», «занятия по
абонементу закончились», «на абонементе осталось N занятий», «абонемент действует
до … — выберите время раньше»); неизвестный токен → `not_found`; `package_id` и
`package_token` вместе → `bad_request`. Имя, email и `cc_email` берутся из пакета,
если не переданы; предоплата не нужна.

**Списание и возврат.** Каждая активная встреча записи по пакету — одно
занятие при записи. Отмена гостем не позже `cancel_hours` до встречи возвращает
занятие, поздняя — нет (в письме об отмене — «занятие списано с абонемента»);
отмена владельцем (`booking.ownerCancel`) возвращает всегда. Остаток 0 → статус
`used` (возврат — снова `active`). Остаток 1 или 0 после списания → гостю (+
копия, Telegram-чаты записей пакета) «Осталось N занятий — продлите абонемент:
<ссылка страницы>» и владельцу `package.low` `{package_id, left}`. Планировщик
переводит `active` в `expired` по `valid_until`.

Пакет — `{status: pending | active | used | expired | cancelled, payment, …}`;
ссылка пакета `<SELFVOY_PUBLIC_URL>/#/p/<токен>` (токен 43 символа base64url; в БД —
sha256 и шифр AES-256-GCM, AAD `selfvoy-package-token`, для писем).

Методы гостя (публичные):

| тип | payload | ответ |
| --- | --- | --- |
| `package.get` | `{token}` | `{package}` — вид гостя: `{title, sessions_total, sessions_left, valid_until ("" — ещё не оплачен), valid_days, status, price, currency, payment (без provider_id; null — без оплаты), receipt_url (чек «Мой налог», "" — нет; F10), payment_note, guest_name, cc_email (замаскирован), page_slug ("" — запись на странице закрыта), page_title, owner_name, owner_timezone, bookings: [{start, status}], branding, ref}` (`ref` — реферальный код владельца, как у `booking.get` / `invite.get`: клиент добавляет его в ссылку «Запись через Selfvoy», даже если `branding.url` пуст); ЮKassa — статус платежа проверяется при каждом открытии; неизвестный токен → `not_found`; 30 в минуту с IP |
| `package.paymentClaim` | `{token}` | `{package}` — «Я оплатил(а)» за пакет (способ `link`): оплата `pending` → `claimed`, покупка `pending` → `active`; владельцу `package.payment` (`claimed`); повтор — без изменений; ЮKassa / нет оплаты → `bad_request`; покупка отменена → `conflict`; лимит — как у переноса/отмены |

Методы владельца:

| тип | payload | ответ |
| --- | --- | --- |
| `package.list` | `{page_id? (0 — все), status? (pending \| active \| used \| expired \| cancelled), limit? (1–200, 50)}` | `{packages: [package]}` — новые сверху |
| `package.issue` | `{page_id, offer_id? \| (title (1–80) + sessions (1–100) + price? (≥ 0, 0) + valid_days? (7–365, 90)), guest_name (1–80), guest_email?, cc_email?, paid? (bool, false)}` | `{package, token, url}` — продажа вне сайта: пакет сразу `active` (`valid_until` = сейчас + `valid_days`), оплата — `paid` (`paid: true`), `pending` (отметит владелец) или null (цена 0); гостю (+ копии) письмо со ссылкой пакета; токен показывается **один раз**; чужая страница → `not_found`; 60 в час |
| `package.adjust` | `{package_id, delta (−100..100, ≠ 0)}` | `{package}` — корректировка остатка (`sessions_total += delta`; убрать больше остатка нельзя → `bad_request`, всего ≤ 1000); статус `used`/`active` — по остатку; отменённый → `conflict` |
| `package.cancel` | `{package_id}` | `{package}` — `cancelled`: по ссылке больше не записаться (сделанные записи остаются — их отменяет `booking.ownerCancel`); ожидаемая оплата → `expired`; оплачен через ЮKassa и ни одно занятие не использовано — возврат всей суммы (`POST /refunds`, `Idempotence-Key: pr-<id>`); гостю письмо; повтор → `conflict` |
| `package.paymentSet` | `{package_id, status: paid \| waived \| refunded}` | `{package}` — отметка оплаты владельцем (`paid_at` / `refunded_at` = сейчас); `paid`/`waived` у `pending` — активация; `paid` — гостю письмо «Оплата пакета получена»; нет оплаты → `bad_request` |

`package` (вид владельца): `{id, page_id, page_title, offer_id (0 — выдан вне
предложений), title, sessions_total, sessions_used, sessions_left, valid_days,
valid_until, price, currency, status, payment (целиком или null), guest_name,
guest_email, cc_email, created_at, updated_at}`. Чужой пакет → `not_found`.

### Лист ожидания и «пора на повторный приём» (F8)

Поля страницы записи (`booking.pageCreate` / `booking.pageUpdate`; есть в виде
владельца `page` и в виде гостя `booking.get`):

| поле | значение |
| --- | --- |
| `waitlist` | bool, `false` — показывать гостю «Встать в лист ожидания» (Pro) |
| `waitlist_hold_minutes` | 15–240, 60 — сколько освободившееся время держится за человеком из листа |
| `rebook_after_days` | 0 (выкл., по умолчанию) или 7–180 — через сколько дней после последней встречи записи напомнить гостю записаться снова (Pro) |

Pro — как у пакетов F5 (при `SELFVOY_PLANS_ENFORCED=1`): включить `waitlist`
(`false` → `true`) или `rebook_after_days` (0 → N) без Pro → `pro_required`;
править и выключать уже включённое можно всегда. Значения вне диапазонов →
`bad_request`.

**Лист ожидания.** Гость оставляет имя, email (обязателен — туда придёт
предложение), желаемые дни и часть дня. Запись листа живёт 30 дней; один email —
одна активная (`waiting` / `offered`) запись на страницу (без учёта регистра;
повтор `booking.waitlistJoin` обновляет имя, дни, часть дня, комментарий и
продлевает срок — место в очереди сохраняется, письма и уведомления повторно не
уходят). Не больше 200 активных записей на страницу (`forbidden` «лист ожидания
этой страницы заполнен»). Ссылка гостя — `<SELFVOY_PUBLIC_URL>/#/w/<токен>` (43
символа base64url; одна на запись листа — и для предложения, и для выхода; в БД —
sha256 и шифр AES-256-GCM, AAD `selfvoy-waitlist-token`, для писем и бота): токен
приходит **только письмом** (подтверждение «Вы в листе ожидания» и каждое
предложение), в ответах API его нет.

Освобождение времени — отмена гостем (`booking.cancel`), владельцем
(`booking.ownerCancel`) или системой (оплата записи/пакета не поступила), перенос
(`booking.reschedule` — прежнее время), выход/удаление записи листа с
предложением, неотвеченное предложение (планировщик). Для каждого освободившегося
интервала сервер под блокировкой расписания владельца (общей для экземпляров на
PostgreSQL) выбирает **самую старую** подходящую запись листа (`waiting`, срок не
истёк): день в `days` (пусто — любой), часть дня по местному времени владельца
(`morning` — до 12:00, `afternoon` — 12:00–17:00, `evening` — с 17:00; `any` —
любое; местное время встречи неизвестно — подходит только `any`); время должно
укладываться в правила страницы: длительность страницы, начало ≥ сейчас +
`waitlist_hold_minutes` + `notice_hours` (гость успевает забрать с запасом),
конец — в окне `days_ahead`, лимит `max_per_day` дня не превышен, время свободно.
Запись листа → `offered`, время держится до `offer.expires_at` = сейчас +
`waitlist_hold_minutes`. **Держание — занятость владельца**: его нет в слотах
`booking.get` / переноса / `booking.slotsPost`, оно занимает место в лимите
дня, `booking.book` других гостей на это время → `conflict` «это время уже
занято», его не займут приглашения и переговоры. Гостю — письмо «Selfvoy:
освободилось время — <страница>» (время встречи, «держится за вами до HH:MM
(<пояс>)», цена и предоплата, кнопка «Записаться на это время» —
`#/w/<токен>`) и, если подписан, сообщение бота с url-кнопкой. Подходящих нет —
время просто освобождается (и сразу видно в `booking.get` этого экземпляра).

Не забрал за время держания — планировщик сервера: предложение `expired`, запись
листа снова `waiting` **в конце очереди** (`queued_at` = момент истечения; срок
записи листа вышел — `expired`), время — следующей подходящей записи (кроме этой),
затем освобождается. Записи листа старше 30 дней → `expired`. На PostgreSQL каждое
истечение обрабатывает один экземпляр (условный переход под блокировкой
расписания), письмо о предложении уходит один раз.

Методы гостя (публичные, без входа):

| тип | payload | ответ |
| --- | --- | --- |
| `booking.waitlistJoin` | `{slug, guest_name (1–80), guest_email (≤ 254, обязателен), days? (["YYYY-MM-DD", …], ≤ 14, местные даты владельца от вчера до конца окна `days_ahead`; повторы убираются; пусто/нет — любой день), part_of_day? (any \| morning \| afternoon \| evening, any), comment? (≤ 500)}` | `{entry: {id, status (waiting \| offered), days, part_of_day, expires_at}, telegram_url}` — `telegram_url`: `https://t.me/<бот>?start=w_<entry_id>_<sig>` (подпись над `token_hash` записи листа) или "" (бот не настроен); новая запись — владельцу `waitlist.joined`, гостю письмо «Вы в листе ожидания» со ссылкой `#/w/<токен>`. Страница без `waitlist` → `forbidden`; на паузе → `conflict`; неизвестный slug → `not_found`; лимиты — как у `booking.book` (10 в минуту и 30 в час с IP, свои счётчики) |
| `booking.waitlistGet` | `{token}` | `{offer: {status, entry_status, start, end, local, expires_at, page_title, page_slug ("" — запись на странице закрыта), owner_name, owner_timezone, duration_minutes, price, currency, prepay, deposit, policy_text, guest_name, days, part_of_day, waitlist_expires_at, telegram_url, telegram_linked, branding, ref}}` — `status`: `offered` (время держится сейчас), `claimed` (записались), `expired` (не забрали вовремя или срок записи листа вышел), `cancelled` (вышли из листа / владелец удалил), `waiting` (предложения ещё не было; `start`/`end`/`expires_at` — ""); `entry_status` — статус записи листа (`waiting \| offered \| booked \| left \| expired`); после истёкшего предложения запись снова `waiting`, а `status` — `expired` с прежним временем, пока не придёт новое предложение; неизвестный токен → `not_found`; 30 в минуту с IP |
| `booking.waitlistClaim` | `{token}` | как `booking.book`: `{booking, token, url, telegram_url, manage_url (= url), payment (вид гостя оплаты или null)}` — та же проверка (под блокировкой расписания: держание ещё за этим гостем, запас до начала, занятость кроме своего держания, лимит дня) и тот же путь оплаты (снимок цены, предоплата F4: ЮKassa или ссылка/реквизиты, письмо «Вы записаны» со ссылкой управления, задача, напоминания); имя, email и комментарий — из записи листа; одна встреча. Запись листа → `booked`. Владельцу — **одно** уведомление `waitlist.booked` (вместо `booking.created`). Время больше не держится (истекло, ушло другому) → `conflict` «время больше не держится за вами — выберите другое»; уже записаны → `conflict`; длительность страницы изменилась → `conflict`; лимиты — как у `booking.book` (общие счётчики) |
| `booking.waitlistLeave` | `{token}` | `{entry}` (как в `waitlistJoin`) — запись листа → `left`; держание снимается, время уходит следующему; уже закрытая — без изменений; лимит — как у переноса/отмены |

Методы владельца:

| тип | payload | ответ |
| --- | --- | --- |
| `booking.waitlist` | `{page_id? (0 — все), status? (waiting \| offered \| booked \| left \| expired), limit? (1–200, 100)}` | `{entries: [{id, page_id, page_title, guest_name, guest_email (замаскирован, как `cc_email` у гостя: `m***@example.com`), days, part_of_day, comment, status, booking_id (0 — нет), telegram_linked, created_at, expires_at, offer? ({start, end, expires_at} — только у `offered`)}]}` — новые сверху |
| `booking.waitlistRemove` | `{entry_id}` | `{entry}` (вид владельца) — запись листа → `left` (держание снимается, время уходит следующему); уже закрытая — без изменений; чужая → `not_found` |

Уведомления владельцу: `waitlist.joined` (`{entry_id, page_id, guest_name, days,
part_of_day}`), `waitlist.booked` (гость забрал предложенное время: `{booking_id,
page_id, start, end, count, guest_name, entry_id}`).

Бот: `/start w_<entry_id>_<sig>` — чат подписывается на предложения этой записи
листа (ответ «Готово: как только на «<страница>» освободится подходящее время,
напишу сюда»); запись закрыта — «уже закрыта»; поддельная ссылка — «Ссылка
недействительна или устарела»; `/stop` снимает и эти подписки.

**«Пора записаться снова».** Если у страницы `rebook_after_days` > 0: когда
последняя активная встреча записи (одиночной или последняя в серии) прошла,
запись активна, а у гостя нет **другой активной записи этой страницы с более
поздним концом** (будущей или более поздней прошедшей; сравнение по email без
учёта регистра), в `конец + rebook_after_days` гостю письмо (копия — `cc_email`;
и в Telegram-чаты, подключённые к этой записи) «Selfvoy: пора записаться снова —
<страница>» со ссылкой «Записаться снова» `<SELFVOY_PUBLIC_URL>/#/b/<slug>` (в
боте — url-кнопкой). **Ровно один раз на запись**: планировщик атомарно
захватывает наступившие (PostgreSQL — `UPDATE … FOR UPDATE SKIP LOCKED`,
`bookings.rebook_sent_at`); не отправилось (есть более поздняя запись, страница
на паузе/удалена, некому писать, опоздали больше чем на 3 дня — сервер стоял) —
запись помечается пропущенной и больше не рассматривается. Выключение поля
(`rebook_after_days: 0`) отменяет неотправленные напоминания прошедших записей
(включение их не возвращает). Вид владельца `booking` (`booking.list`,
`booking.ownerCancel`, `booking.paymentSet`, `tax.receiptDone`):
`rebook_due_at` — когда уйдёт напоминание ("" — не запланировано: поле
выключено, запись отменена или уже обработана), `rebook_sent_at` — когда ушло
("" — не отправлялось).

### Чек «Мой налог» (F10, самозанятые)

Настройка владельца — `prefs.set` в **корне** payload: `npd_receipts` (bool,
`false`; «я самозанятый, напоминать выдать чек»; не bool → `bad_request`;
включение без Pro при `SELFVOY_PLANS_ENFORCED=1` → `pro_required`, выключить можно
всегда). `prefs.get` всегда отдаёт `npd_receipts`.

Когда оплата записи или пакета становится `paid` (ЮKassa — планировщик /
`booking.manage` / `package.get`; отметка владельца `booking.paymentSet` /
`package.paymentSet`; `package.issue` с `paid: true`), валюта `RUB`, сумма > 0 и
`npd_receipts` включено — владельцу уведомление `tax.receipt` и задача «Выдать
чек: <услуга>, <сумма>» (срок `due_at` — сегодня 23:59 по поясу владельца;
напоминание — за 3 часа до срока, не раньше «сейчас»; пояс — через AI-сервис, без
него — по местному времени встречи, иначе UTC). В `payment` записи/пакета
появляются `npd: true` и `receipt_task_id` (повторная отметка `paid` второй
задачи не создаёт). Payload `tax.receipt`: `{booking_id | package_id, page_id,
service, amount, currency: "RUB", client (имя гостя), paid_at, text, url:
"https://lknpd.nalog.ru/", task_id}`; `text` — готово для копирования в «Мой
налог»: `Наименование: <услуга>; Сумма: <сумма>; Покупатель: физическое лицо`;
`service` — название страницы (пакет — «<страница>: абонемент «<пакет>»»);
сумма — то, что реально получено (`payment.amount`: депозит, полная предоплата или
цена пакета; в `text` — «2 500 ₽»).

| тип | payload | ответ |
| --- | --- | --- |
| `tax.pending` | `{limit? (1–200, 50)}` | `{items: [{kind: booking \| package, id, service, amount, currency, client, paid_at, text, url}]}` — оплаты с `npd: true`, `status: paid`, чек не отмечен (`receipt_at` пуст); новые сверху |
| `tax.receiptDone` | `{booking_id \| package_id (ровно одно), receipt_url? (https://, ≤ 500)}` | `{booking}` или `{package}` (вид владельца) — в `payment` пишутся `receipt_url` ("" — без ссылки) и `receipt_at` (сейчас), задача «Выдать чек» закрывается (`done`); повтор обновляет ссылку. Нет id или оба → `bad_request`; ссылка не https / длиннее 500 → `bad_request`; у записи нет оплаты → `bad_request`; оплата не `paid` → `conflict`; чужая → `not_found` |

Гость видит ссылку «Чек» в виде записи (`booking.manage` и др.: поле
`receipt_url`, и `payment.receipt_url`) и пакета (`package.get`: `receipt_url`),
если `receipt_url` задан. Служебные `npd`, `receipt_task_id`,
`receipt_refund_task_id` гостю не отдаются (вид владельца — `payment` целиком).

Возврат (`refunded`: `booking.paymentSet` / `package.paymentSet`, автоматический
возврат ЮKassa) оплаты с отмеченным чеком (`receipt_at` задан) — уведомление
`tax.refund` «Аннулируйте чек в «Мой налог»» (`{booking_id | package_id, page_id,
service, amount, currency, client, receipt_url, refunded_at, url, task_id}`) и
задача «Аннулировать чек: <услуга>, <сумма>»; один раз на оплату
(`payment.receipt_refund_task_id`). Без отмеченного чека возврат `tax.refund` не
создаёт.

### Пост «свободные окна» (F6)

Владелец копирует готовый текст с реальными свободными окнами страницы записи
в Telegram-канал, VK или куда угодно. Окна — те же, что гость видит в
`booking.get` (кандидаты AI-сервиса + запас до начала, занятость владельца,
лимит записей в день).

| тип | payload | ответ |
| --- | --- | --- |
| `booking.slotsPost` | `{page_id, days? (1–14, 7), format? ("telegram" \| "vk" \| "plain", telegram)}` | `{text, slots (число окон в тексте), url (ссылка страницы или "")}` |

```
Свободные окна: Консультация (50 мин, 2 500 ₽)

Пн, 21 окт: 10:00, 11:30, 15:00
Вт, 22 окт: 12:00

Записаться: https://selfvoy.example.com/#/b/<slug>
```

- Время — местное время владельца (`local` слотов AI); если у какого-то окна
  его нет, все окна в UTC и в заголовке — «, время UTC».
- В строке дня не больше 8 времён, остальные — «и ещё N».
- Нет окон — «На ближайшие N дней свободных окон нет» (и ссылка).
- `vk` — то же, ссылка строкой «Запись онлайн: …»; `plain` — без пустых строк.
- Цена в заголовке — только если у страницы задана цена (поле `price`, F4;
  пока его нет — заголовок без цены).
- Ссылка — `<SELFVOY_PUBLIC_URL>/#/b/<slug>`; без `SELFVOY_PUBLIC_URL` строки
  со ссылкой нет, `url` пуст (клиент добавляет адрес сам).

Ошибки: `not_found` — нет страницы, чужая или удалённая; `conflict` — запись
на странице на паузе; `forbidden` — email владельца не подтверждён или больше
30 постов в час; `bad_request` — неверные `days`/`format`; `upstream_error` —
AI-сервис недоступен.

### Календарь: ICS-подписка (F6)

Владелец подписывается на свои встречи в Google/Apple Календаре, Outlook
(«Добавить календарь по URL»).

| тип | payload | ответ |
| --- | --- | --- |
| `calendar.feed` | — | `{enabled, url, path}` — `url`/`path` всегда пусты: токен не хранится |
| `calendar.feedReset` | — | `{enabled: true, url, path}` — новая ссылка, прежняя перестаёт работать; токен виден только в этом ответе; больше 20 в час → `forbidden` |
| `calendar.feedDisable` | — | `{enabled: false}` — ссылка перестаёт работать |

`path` = `/ics/<токен>.ics` (токен — 43 символа base64url; в БД только sha256,
`users.ics_token_hash`). `url` = `SELFVOY_SERVER_PUBLIC_URL` + `path` (внешний
адрес сервера, например `https://api.example.com`); без переменной `url` = "",
клиент собирает адрес из адреса сервера: `wss://host` → `https://host`,
`ws://host` → `http://host`.

HTTP на том же порту, что WebSocket (обычный запрос без `Upgrade: websocket`):

| запрос | ответ |
| --- | --- |
| `GET /healthz` | `200 application/json` `{"status":"ok"}` — только факт работы |
| `GET /ics/<токен>.ics` | `200 text/calendar; charset=utf-8`, `Cache-Control: no-store` |
| неизвестный/выключенный/старый токен, владелец заблокирован | `404` |
| другой метод на `/ics/…` | `405`, `Allow: GET` |
| больше 60 запросов `/ics/…` в минуту с одного IP | `429`, `Retry-After: 60` |
| прочие пути без `Upgrade` | `400` (как раньше) |

IP клиента за доверенным прокси — из `X-Forwarded-For` (как у WebSocket,
`SELFVOY_TRUSTED_PROXIES`); на PostgreSQL лимит общий для всех экземпляров
(`rate_limits`).

Содержимое (RFC 5545): `VCALENDAR` с `X-WR-CALNAME:Selfvoy`, `METHOD:PUBLISH`,
`REFRESH-INTERVAL`/`X-PUBLISHED-TTL` = 1 час; `VEVENT` — активные встречи
записей владельца и принятые приглашения со стартом от −30 до +180 суток:

- `UID:booking-<id>-<start unix>@selfvoy` (приглашения — `invite-<id>-<start unix>@selfvoy`);
- `DTSTART`/`DTEND` в UTC (`20261021T070000Z`), `DTSTAMP` — время последнего
  изменения записи (ответа на приглашение);
- `SUMMARY` — «<страница>: <имя гостя>» (приглашение — «<тема>: <имя гостя>»);
- `DESCRIPTION` — комментарий гостя (если есть);
- `STATUS:CONFIRMED`, `TRANSP:OPAQUE`.

Строки — CRLF; TEXT экранируется (`\\`, `\;`, `\,`, перевод строки → `\n`,
прочие управляющие символы выбрасываются); строки длиннее 75 октетов
складываются (продолжение с пробела) без разрыва символов UTF-8. Email гостя
в календарь не попадает.

### Рефералы

| тип | payload | ответ |
| --- | --- | --- |
| `referral.get` | `{limit? (1–100, 50)}` | `{code, url, joined, verified, rewarded, reward_days: 30, reward_cap: 12, plan, referrals: [{display_name, source, joined_at, verified}]}` |

У пользователя один код из 8 символов `[A-Z0-9]` (выдаётся при первом
запросе); `url` — `<SELFVOY_PUBLIC_URL>/#/signup?ref=<код>` или пусто. Код
передаётся в `auth.register` (`ref`, `ref_source`); приглашённый
засчитывается (`verified`) после `auth.verifyEmail`, тогда пригласившему
приходит уведомление `referral.joined` (`{display_name, reward_days}`). Email и id
приглашённых пригласившему не раскрываются.

Награда: за подтверждённого приглашённого обоим начисляется 30 дней Pro
(`plan_until = greatest(now, plan_until) + 30 дней`) — один раз
(`referrals.rewarded_at`). Пригласивший получает награду максимум за 12
приглашённых (дальше растёт только счётчик `verified`), приглашённый — всегда.
В тело `referral.joined` добавляется «· +1 месяц Pro», если пригласившему
начислено; приглашённому — уведомление `referral.reward` «Месяц Pro в подарок»
(`{days, until}`). `rewarded` — число начисленных пригласившему наград (≤ 12),
`plan` — как в объекте `user`. Код владельца есть и в виде гостя
приглашения (`invite.get` → `ref`) и страницы записи.

### Telegram-бот

Бот включается на сервере переменными `SELFVOY_TELEGRAM_BOT_TOKEN` и
`SELFVOY_TELEGRAM_BOT_USERNAME`; его имя отдаёт `server.info` → `telegram_bot`
("" — бот не настроен; есть и в ответе без входа). Привязка — по deep link
`https://t.me/<бот>?start=<payload>`, payload (≤ 64 символов `[A-Za-z0-9_-]`)
подписан сервером (HMAC-SHA256, 16 байт подписи в base64url):
`o_<user_id>_<expires_unix>_<sig>` — владелец, ссылка живёт 15 минут;
`g_<booking_id>_<sig>` — гость записи, подпись над `manage_hash` (после
перевыпуска ссылки управления прежняя ссылка бота не работает);
`w_<entry_id>_<sig>` — гость листа ожидания (F8), подпись над `token_hash` записи
листа: предложения освободившегося времени приходят в этот чат.

| тип | payload | ответ |
| --- | --- | --- |
| `telegram.status` | — | `{enabled, bot_username, linked, username}` (`username` — ник в Telegram или "") |
| `telegram.link` | — | `{url, expires_at}` — ссылка владельца на 15 минут; бот не настроен → `bad_request` «Telegram-бот не настроен на сервере» |
| `telegram.unlink` | — | `{linked: false}` |

Команды бота (только личные чаты): `/start o_…` — чат привязывается к аккаунту
(если он был привязан к другому — перепривязывается); `/start g_…` — к записи
(запись активна, не больше 3 чатов), ответ — ближайшая встреча в местном времени
владельца; `/start w_…` — подписка на предложения листа ожидания (F8); `/start p_…` — чат гостя
привязывается к странице ИИ-администратора (F7, ниже); `/start` без payload и любой текст — справка; `/stop` — чат
отвязывается от аккаунта, от всех записей, подписок листа ожидания и страницы ИИ-администратора. Поддельная или просроченная ссылка —
ответ «Ссылка недействительна или устарела». Если пользователь заблокировал бота
(Telegram отвечает 403), чат отвязывается везде.

Владельцу в привязанный чат дублируются уведомления видов `booking.*`,
`package.*`, `invite.*`, `referral.*`, `waitlist.*`, `tax.*`, `task.due`, `telegram.question` (F7) текстом «<title>\n<body>» —
если вид не в `prefs.notifications.muted_kinds`, сейчас не тихие часы и
`prefs.notifications.telegram` не `false`. Гостю — те же извещения, что письмом
(см. «Страницы записи»). Кнопка «Приду» (callback `cf:<booking_id>:<start_unix>`)
принимается только из чата, привязанного к этой записи, и подтверждает встречу
так же, как `booking.confirm` (ответ «Спасибо, ждём вас», клавиатура сообщения
убирается; встреча прошла или отменена — «встреча уже прошла или отменена»).

### ИИ-администратор в Telegram (F7)

Бот отвечает гостям страницы записи на вопросы о цене и условиях, показывает
свободное время и записывает — от имени владельца, 24/7. Нужен настроенный
Telegram-бот (см. «Telegram-бот»); функция владельца — Pro (`pro_required` при
`SELFVOY_PLANS_ENFORCED=1`).

Поле страницы (`booking.pageCreate` / `booking.pageUpdate`):

| поле | тип | описание |
| --- | --- | --- |
| `ai_admin` | bool, `false` | включить ИИ-администратора. Не bool → `bad_request`; бот не настроен на сервере → `bad_request` «ИИ-администратор отвечает гостям через Telegram-бота, а бот не настроен на сервере…»; включить без Pro (при `SELFVOY_PLANS_ENFORCED=1`) → `pro_required`; выключить и сохранить уже включённое можно всегда |

Вид владельца `page` — `ai_admin` и `telegram_ask_url`; вид гостя `booking.get` —
`telegram_ask_url`: `https://t.me/<бот>?start=p_<slug>` (кнопка «Спросить в
Telegram»), `""` — поле выключено, бот не настроен, у владельца нет Pro (при
`SELFVOY_PLANS_ENFORCED=1`) или страница на паузе.

**Бот.** `/start p_<slug>` привязывает личный чат к странице (один чат — одна
страница; повтор с другой страницы перепривязывает и начинает разговор заново).
Неизвестная/удалённая страница — «Страница записи не найдена»; администратор
выключен или нет Pro — «У страницы «…» нет ИИ-администратора в Telegram» и ссылка
на страницу; пауза — «Запись … сейчас закрыта». Хранится по минимуму: имя
(`first_name` + `last_name`) и ник из Telegram, последние ≤ 10 сообщений (контекст
ИИ). `/stop` и блокировка бота (403) удаляют привязку. Текст гостя (длиннее 1000
символов обрезается) → сервер вызывает AI `POST /v1/agent/admin` (ниже) с
ближайшими ≤ 20 свободными временами — теми же, что гость видит в `booking.get`.

Сервер не доверяет ответу ИИ: `reply` — без управляющих символов, не длиннее
1000 символов; в `slots` остаются только времена из `free_slots` (без повторов,
не больше 6); неизвестный `action` → `none`; `offer_slots`/`book` без времени →
`none`; пустой ответ или ошибка AI-сервиса → «Сейчас не получается ответить
автоматически. Записаться можно на странице: <url>» (+ url-кнопка, если адрес https).

- `none` — бот отправляет `reply`.
- `offer_slots` / `book` — `reply` и inline-кнопки времени
  («чт, 15 октября, 10:30», местное время владельца). Бот никогда не записывает
  сам: нажатие кнопки → вопрос «Записать вас на <время>? … Имя для записи: <имя>»
  (цена и предоплата, если есть) с кнопками «Да, записать» / «Нет»; запись —
  только по второму нажатию, **обычным путём `booking.book`** (те же проверки
  свободного времени, лимита дня, оплаты; задачи и уведомление `booking.created`
  владельцу; имя — из Telegram, email нет, `guest_comment` — «Запись через
  Telegram-бота (@ник)»; лимиты частоты записи — на чат). Чат привязывается к
  записи (как `g_…`): извещение «Вы записаны» с кнопкой «Перенести или отменить»
  и напоминания приходят сюда. Нужна предоплата — бот присылает сумму, срок,
  ссылку оплаты (ЮKassa или `payment_url`, url-кнопкой) и реквизиты
  (`payment_note`). Время уже заняли — ответ кнопки «Это время уже заняли —
  выберите другое».
- `escalate` — вопрос сохраняется и владельцу приходит уведомление
  `telegram.question` (in-app, push и Telegram владельца): payload
  `{question_id, page_id, guest_name, telegram_username, text}`; гостю — «Передал
  специалисту, он ответит вам здесь или напишет.»

callback_data кнопок (≤ 64 байт) подписаны сервером и привязаны к чату:
`as:<page_id>:<start_unix>:<sig>` (выбор времени) и `ab:<page_id>:<start_unix>:<sig>`
(«Да, записать»), `sig` — HMAC-SHA256 (16 байт, base64url) над
`<as|ab>:<chat_id>:<page_id>:<start_unix>`; `an` — «Нет». Из чужого чата или с
изменёнными данными — «Эта кнопка — не для этого чата»; чат перепривязан к
другой странице — «Кнопка устарела».

Лимиты: 30 сообщений гостя в сутки на чат, 300 вызовов ИИ в сутки на владельца
(превышение — ответ со ссылкой на страницу записи, ИИ не вызывается), 5 вопросов
специалисту в сутки на чат (дальше — «Я уже передал ваши вопросы специалисту…»).
Тексты гостей в лог сервера не пишутся.

Методы владельца (нужен вход):

| тип | payload | ответ |
| --- | --- | --- |
| `telegram.questions` | `{status? ("" \| open \| answered), limit? (1–200, 50)}` | `{questions: [question]}` — новые сверху; неверные `status`/`limit` → `bad_request` |
| `telegram.reply` | `{question_id, text (1–1000)}` | `{question}` — бот отправляет гостю «<имя специалиста>: <text>», вопрос → `answered` (повторный ответ допускается, `answer` — последний); нет вопроса или чужой → `not_found`; пустой/длинный текст → `bad_request`; гость отключил бота (`/stop`, блокировка) → `conflict`; бот не настроен → `bad_request`; больше 120 ответов в час → `forbidden` |

`question`: `{id, page_id, page_title (только в списке), guest_name,
telegram_username ("" — нет ника), text, status (open | answered), answer,
created_at, answered_at ("" — не отвечен), can_reply (бот настроен и гость не
отключил его)}`.

### Администрирование

Роли (`user.role`): `user` (по умолчанию), `moderator`, `admin`. Роль
приходит в каждом `user` (`auth.login`, `auth.login2fa`, `auth.token`,
`auth.me`, `auth.verifyEmail`), но права проверяет **сервер** — по роли из
БД на каждый запрос, поэтому понижение действует сразу, без перевхода.
Методы `admin.*` требуют входа (без него — `unauthorized`).

| | moderator | admin |
| --- | --- | --- |
| `admin.stats`, `admin.users`, `admin.user` | да | да |
| `admin.setActive`, `admin.revokeSessions` | только над `role=user` | над любым, кроме себя |
| `admin.setRole`, `admin.audit` | нет (`forbidden`) | да |

Первые администраторы — env **`SELFVOY_ADMIN_EMAILS`** (адреса через запятую,
`;` или пробел, без учёта регистра): пользователь с адресом из списка
получает `admin` при старте сервера и при успешных `auth.login` /
`auth.login2fa` / `auth.verifyEmail` — **только если email подтверждён**
(иначе роль досталась бы тому, кто первым зарегистрировал чужой адрес).
Список — источник истины: снятая вручную роль вернётся при следующем входе,
пока адрес в списке. Каждое назначение пишется в журнал (`role_bootstrap`).

`AdminUser` = `id`, `email`, `display_name`, `avatar_url`, `role`,
`is_active`, `email_verified`, `two_factor`, `created_at`, `last_seen_at`,
`plan` (как в `user`; хэш пароля и токены не отдаются).

| тип | payload | ответ |
| --- | --- | --- |
| `admin.stats` | — | `users_total`, `users_verified`, `users_disabled`, `users_2fa`, `users_new_7d`, `moderators`, `admins`, `sessions_active` (не отозваны и не истекли), `online_now` (разных пользователей с живым соединением) |
| `admin.users` | `query?` (подстрока email или имени, ≤100 симв.), `role?` (`''`\|`user`\|`moderator`\|`admin`), `status?` (`''`\|`active`\|`disabled`\|`unverified`), `offset?`, `limit?` (1–100, по умолчанию 50) | `users[]` (AdminUser, новые первыми), `total` (без учёта `offset`/`limit`); неизвестные `role`/`status` → `bad_request` |
| `admin.user` | `user_id` | `user` (AdminUser), `sessions_active`, `audit[]` (30 последних записей журнала пользователя; модератору — без `remote_addr`); нет такого — `not_found` |
| `admin.setActive` | `user_id`, `active` (bool), `reason` (обязательна при блокировке, ≤200 симв.) | `user`; блокировка (`active=false`) отзывает все сессии и refresh-токены и закрывает открытые соединения пользователя (WS close `4001`, причина `account_disabled`); заблокированный не входит (`auth.login` → `forbidden`) и не проходит по старому токену (`unauthorized`) |
| `admin.setRole` | `user_id`, `role` | `user`; свою роль менять нельзя, последнего активного администратора понизить нельзя (`forbidden`); `moderator`/`admin` — только активному пользователю с подтверждённым email (`bad_request`) |
| `admin.revokeSessions` | `user_id` | `revoked` (сколько живых сессий завершено); refresh-токены отозваны, соединения закрыты (WS close `4001`, причина `sessions_revoked`); аккаунт не блокируется — войти заново можно |
| `admin.setPlan` | `user_id`, `days` (целое 0–3650; 0 — сбросить в Free) | `user` (AdminUser + `plan`); только admin (`forbidden`), `plan_until = now + days`; запись `admin_plan_changed` (`days`, `plan_until`, `old_plan_until`, `actor_email` \| `target_email`) в журнал; неверный `days` → `bad_request` |
| `admin.audit` | `user_id?` (0 — все), `before_id?` (0 — с самых новых; иначе записи с `id < before_id`), `limit?` (1–200, по умолчанию 100) | `entries[]`: `id`, `user_id`, `user_email?`, `kind`, `detail`, `remote_addr`, `created_at` — новые первыми; следующая страница — `before_id` = `id` последней записи |

Ошибки: `forbidden` — не хватает роли / цель выше по роли / действие над
собой; `not_found` — нет пользователя; `bad_request` — неверные поля.

Журнал (`audit_logs.kind`) действий панели; запись пишется и цели, и актору:
`admin_user_disabled` / `admin_user_enabled` (`detail`: `actor_id`,
`actor_email` | `target_id`, `target_email`; `reason`; `revoked` при
блокировке), `admin_role_changed` (`role`, `old_role`, `actor_email` |
`target_email`), `admin_sessions_revoked` (`revoked`, `actor_email` |
`target_email`), `role_bootstrap` (`role`).

## HTTP API Python AI Service

| метод | путь | назначение |
| --- | --- | --- |
| `POST` | `/v1/agent/run` | главный запрос к Selfvoy |
| `POST` | `/v1/agent/negotiate` | Agent-to-Agent переговоры |
| `POST` | `/v1/agent/slots` | слоты владельца для приглашения по ссылке: `{context, duration_minutes (15–240), window_hours, max_slots (1–12)}` → `{slots: [{start, end, score, reason}]}` |
| `POST` | `/v1/agent/admin` | ИИ-администратор в Telegram (F7): `{page: {title, description, duration_minutes, price, currency, prepay, deposit, policy_text, cancel_hours, timezone, owner_name, price_text?, deposit_text?}, free_slots: [start] (≤ 20), history: [{role: guest \| admin, text}] (≤ 20), message}` → `{reply (≤ 1000), action: none \| offer_slots \| book \| escalate, slots: [start], escalate_reason, source: llm \| fallback}`. Без LLM — запасной режим по ключевым словам: отмена/перенос → где найти ссылку «Перенести или отменить»; цена/стоимость/сколько → цена, предоплата и политика отмены (цены нет → `escalate`); запись/время/когда/свободно → `offer_slots` с ближайшими 3 временами; приветствие/спасибо → `none`; остальное → `escalate`. С LLM — JSON mode, ответ проверяется pydantic (неверные типы, лишняя длина, неизвестное действие, время не из `free_slots`) — при ошибке запасной режим |
| `POST` | `/v1/speech/transcribe` | распознавание речи (Whisper-совместимое, ru/en/auto) |
| `POST` | `/v1/memory/extract` | извлечение фактов в память |
| `GET` | `/v1/memory/{user_key}` | записи памяти (можно `?query=` — гибридный RAG) |
| `POST` | `/v1/planner/slots` | свободные слоты |
| `POST` | `/v1/planner/understand` | разбор фразы (намерение, окно, длительность) |
| `GET` | `/v1/tools` | список инструментов |
| `GET` | `/healthz` | состояние (провайдер LLM, провайдер STT, бэкенды) |

Все запросы, кроме `/healthz`, требуют заголовок `X-Selfvoy-Token` со значением
`SELFVOY_AI_TOKEN`. Без настроенного токена сервис отвечает `503` (fail closed),
кроме локальной разработки с `SELFVOY_AI_INSECURE_DEV=1`. `/healthz` отдаёт
только типы бэкендов (без DSN и путей). `POST /v1/agent/run`: `execute`
по умолчанию `false` — AI только планирует; даже при `execute: true`
действия с внешним эффектом (`send_email`, `send_message`, `book_table`) не
исполняются (`results[].error = "server_only: …"`) — их выполняет C++-сервер.
Ответ LLM проверяется: `confidence` — число 0..1, `memory_updates` — не больше
5 записей вида `fact|preference|schedule|contact` с текстом до 300 символов,
`actions[].args` — только объект. Некорректный ввод — `4xx`, не `500`.
Swagger — `/docs`.

### Гибридный RAG памяти (этап 8)

Ранжирование воспоминаний под запрос (`memory.rank`, используется в
`load_context_memory` и `GET /v1/memory/{user_key}?query=`) — гибридное:

* **лексика (Okapi BM25)** по основам токенов + буст весом записи — работает
  всегда, без внешних зависимостей;
* **семантика** по косинусной близости эмбеддингов — включается, когда задан
  эндпоинт эмбеддингов; итоговый порядок объединяется **Reciprocal Rank Fusion**;
* **graceful degrade**: если эмбеддинги не настроены или недоступны (ошибка
  сети/формата), ранжирование остаётся лексическим и не падает.

Настройки AI-сервиса: `SELFVOY_RAG_MODE` (`lexical` | `hybrid` | `auto`, по
умолчанию `auto` — семантика включается при заданном `SELFVOY_EMBEDDINGS_URL`),
`SELFVOY_EMBEDDINGS_URL` (OpenAI-совместимый `POST /embeddings`),
`SELFVOY_EMBEDDINGS_MODEL` (по умолч. `text-embedding-3-small`),
`SELFVOY_EMBEDDINGS_API_KEY`, `SELFVOY_EMBEDDINGS_TIMEOUT`. Без `SELFVOY_EMBEDDINGS_URL`
гибрид выключен — используется чистый BM25.

## Как выглядит A2A-ответ

```json
{
  "accepted": true,
  "slot": {"start": "2026-09-19T10:00:00+00:00", "end": "2026-09-19T11:00:00+00:00",
           "score": 0.72, "reason": "общее окно в графиках"},
  "place": {"name": "Кофе на полпути", "city": "Керкраде", "rating": 4.7, "tags": ["vegan"]},
  "message": "Да, 19.09 в 10:00 подходит. Предлагаю Кофе на полпути."
}
```
