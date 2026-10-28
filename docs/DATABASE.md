# База данных Selfvoy

PostgreSQL 14+ (прод) или встроенное JSON-хранилище (разработка/тесты,
`SELFVOY_EMBEDDED_DB`). Оба бэкенда реализуют один интерфейс `IDatabase`
(`server/include/selfvoy/idatabase.h`); C++-тесты работают на встроенном,
e2e — на реальном PostgreSQL.

Схема: `schema/schema.sql` (идемпотентна, `CREATE … IF NOT EXISTS`);
изменения — файлами `schema/migrations/` (применяются по порядку имени).
Демо-данные: `schema/seed.sql`.

## 34 таблицы и 2 представления по доменам

### Аутентификация и безопасность
| таблица | назначение |
| --- | --- |
| `users` | аккаунты: email (уникальный, нормализованный), хэш пароля Argon2id, `email_verified`, `active`, `role` (`user`/`moderator`/`admin`, админ-панель; миграция `2026_10_13_roles.sql`) |
| `sessions` | живые сессии: jwt_id, устройство, адрес, `expires_at`, `revoked_at` (отзыв мгновенный) |
| `auth_tokens` | одноразовые коды (подтверждение email, сброс пароля) — только SHA-256 хэш, TTL |
| `refresh_tokens` | refresh-пары с ротацией: хэш, `family`, отозванные переиспользования |
| `audit_logs` | журнал событий безопасности (вход, блокировка, 2FA, отзывы) |
| `two_factor_settings` | TOTP-секрет (зашифрован AES-256-GCM), статус, счётчик окон |
| `recovery_codes` | резервные коды 2FA — хэши, одноразовые |
| `trusted_devices` | устройства, доверенные для входа без 2FA (по device_id, TTL) |

### Общение
| таблица | назначение |
| --- | --- |
| `chats` | диалоги: `kind` CHECK (`direct`/`group`/`agent`) |
| `chat_members` | участники + роль |
| `messages` | сообщения: `kind` CHECK (`text`/`agent_action`/`agent_reply`/`system`), payload JSONB |

### Память и настройки
| таблица | назначение |
| --- | --- |
| `user_memory` | факты/предпочтения/расписание/контакты: `kind` CHECK (`fact`/`preference`/`schedule`/`contact`), текст, вес |
| `user_preferences` | JSONB-настройки (тема, диета, город, бюджет, `notifications.quiet_hours`, `notifications.muted_kinds`); онбординг-анкета: `birthday`, `allergies[]`, `onboarded` (миграция `2026_09_21_onboarding.sql`) |

### Инструменты и задачи
| таблица | назначение |
| --- | --- |
| `tool_permissions` | режимы инструментов на пользователя: `allow`/`ask`/`deny` |
| `pending_actions` | отложенные опасные действия (барьер подтверждения): аргументы, `expires_at`, статус |
| `tasks` | задачи/напоминания: статус, приоритет, `remind_at`, `reminded_at` |

### Интеграции
| таблица | назначение |
| --- | --- |
| `integration_connections` | подключения Google: access/refresh токены **только шифром** (AES-256-GCM), scope, статус |
| `integration_oauth_states` | одноразовые state + PKCE verifier (TTL `SELFVOY_OAUTH_STATE_TTL`) |

### Уведомления (этап 13)
| таблица | назначение |
| --- | --- |
| `notifications` | in-app «входящая»: kind, заголовок, тело, payload JSONB, `read_at`; индексы (user_id, id DESC) и частичный по непрочитанным |
| `push_devices` | токены push-доставки: `platform` CHECK (`apns`/`fcm`/`webhook`/`dev`), UNIQUE(user_id, token), `enabled`, `last_used_at`; `fcm` добавлена миграцией `2026_09_20_push_fcm.sql` (этап 11) |

### Приглашения по ссылке
| таблица | назначение |
| --- | --- |
| `meeting_invites` | встреча с человеком без Selfvoy (`invite.*`): `owner_id`, `token_hash` UNIQUE (sha256 токена ссылки — сам токен не хранится), тема, заметка, `duration_minutes` CHECK 15–240, `slots` JSONB `[{start,end}]`, `status` CHECK (`open`/`accepted`/`declined`/`cancelled`; `expired` вычисляется по `expires_at`), ответ гостя (`guest_name/email/comment`, `chosen` JSONB), `responded_at`; индекс (owner_id, id DESC). Миграция `2026_10_14_invites.sql` |

### Страницы записи и рефералы
| таблица | назначение |
| --- | --- |
| `booking_pages` | постоянная ссылка владельца (`booking.*`): `slug` UNIQUE (10 символов), название, описание, `duration_minutes` CHECK 15–240, `days_ahead` 1–60, `max_per_day` 1–16, `notice_hours` 0–168, `max_series` 1–12, `weekends`, `status` CHECK (`active`/`paused`/`archived`); индекс (owner_id, id DESC) |
| `bookings` | запись гостя: `page_id`, `owner_id`, `manage_hash` UNIQUE (sha256 токена ссылки управления), `guest_name/email/comment`, `occurrences` JSONB `[{start,end,day,local,status,task_id,confirmed_at?}]`, `status` CHECK (`active`/`cancelled`), `cancelled_by`, `reschedules`, `ends_at` (конец последней активной встречи; занятость и лимит в день читают только предстоящие записи), `price` INTEGER (снимок цены одной встречи на момент записи, мин. единицы; 0 — по пакету или бесплатно) и `currency` (миграция `2026_10_17_booking_price.sql`); индексы (owner_id, id DESC), (page_id, status), частичные (owner_id, ends_at) и (page_id, ends_at) по активным |
| `referral_codes` | реферальный код пользователя: `user_id` PK, `code` UNIQUE CHECK `^[A-Z0-9]{8}$` |
| `referrals` | кто кого пригласил: `referrer_id`, `referred_id` UNIQUE (один пригласивший), `source` CHECK (`link`/`booking`/`invite`), `verified_at` (после подтверждения email); индекс (referrer_id, id DESC) |

Миграция `2026_10_14_booking_referrals.sql`.

### Несколько экземпляров сервера
| таблица | назначение |
| --- | --- |
| `rate_limits` | общие счётчики попыток (вход, коды, 2FA, лимиты гостевых методов): `key` PK, `count`, окно `window_start` + `window_sec`; атомарный UPSERT, истёкшие строки сервер периодически удаляет порциями по индексу `window_start`. Таблица UNLOGGED (без WAL: попытка входа не ждёт fsync; после аварийного рестарта PostgreSQL счётчики обнуляются). Миграции `2026_10_14_rate_limits.sql`, `2026_10_15_audit_fixes.sql` |

Расписание владельца на время «проверить, что свободно → записать встречу»
(`invite.respond`, `booking.*`) блокируется advisory lock'ом PostgreSQL по id
владельца — два экземпляра сервера не займут одно время. Сервер держит пул
соединений (`SELFVOY_DB_POOL_SIZE`, по умолчанию 8) и отдельное соединение для
advisory lock'ов (их берут и отпускают в одной сессии PostgreSQL).

### Рост: тариф, Telegram, служебное состояние
| таблица | назначение |
| --- | --- |
| `booking_telegram` | Telegram-чаты гостя записи (напоминания и извещения): `booking_id` (ON DELETE CASCADE), `chat_id`, `linked_at`; PK (booking_id, chat_id), индекс по `chat_id` (отвязка `/stop` и при 403); не больше 3 чатов на запись — проверяет сервер |
| `server_state` | служебное состояние сервера ключ → значение (`telegram.offset` — смещение `getUpdates`), `updated_at` |
| `payment_accounts` | магазин ЮKassa владельца (F4): `user_id` PK (ON DELETE CASCADE), `provider` CHECK (`yookassa`), `shop_id`, `secret_enc` (AES-256-GCM, AAD `selfvoy-payment-secret`), `connected_at` |
| `package_purchases` | пакеты (абонементы) гостей (F5): `owner_id`, `page_id` (ON DELETE CASCADE), `offer_id` (id предложения страницы; 0 — выдан владельцем), снимок предложения `title`, `sessions_total` CHECK 1–1000, `sessions_used`, `valid_days`, `price`, `currency`; `valid_until` (NULL — ждёт оплату), `status` CHECK (`pending`/`active`/`used`/`expired`/`cancelled`), `payment` JSONB (как `bookings.payment`, `kind: package`), `guest_name/email`, `cc_email`, `token_hash` UNIQUE (sha256 токена ссылки `#/p/<токен>`), `token_enc` (тот же токен, AES-256-GCM, AAD `selfvoy-package-token` — для писем); индексы (owner_id, id DESC), частичные по `payment->>'due_at' WHERE status = 'pending'` и `valid_until WHERE status = 'active'` |
| `waitlist_entries` | лист ожидания страницы записи (F8): `owner_id`, `page_id` (ON DELETE CASCADE), `guest_name`, `guest_email`, `days` JSONB (местные даты владельца; `[]` — любой день), `part_of_day` CHECK (`any`/`morning`/`afternoon`/`evening`), `comment`, `status` CHECK (`waiting`/`offered`/`booked`/`left`/`expired`), `token_hash` UNIQUE (sha256 токена ссылки `#/w/<токен>`), `token_enc` (тот же токен, AES-256-GCM, AAD `selfvoy-waitlist-token`), `telegram_chat_id` (подписка в боте; NULL — нет), предложение `offer_start` / `offer_end` / `offer_day` / `offer_local` / `offer_expires_at` / `offer_status` CHECK (`''`/`offered`/`claimed`/`expired`/`cancelled`), `booking_id` (ON DELETE SET NULL), `queued_at` (место в очереди), `expires_at` (+30 дней); уникальный частичный индекс `(page_id, lower(guest_email)) WHERE status IN ('waiting','offered')` (один email — одна активная запись), индексы (owner_id, id DESC), очередь `(page_id, queued_at) WHERE waiting`, держания `(owner_id, offer_expires_at) WHERE offered`, планировщик `offer_expires_at WHERE offered` и `expires_at WHERE waiting`, `telegram_chat_id` |
| `booking_reminders` | напоминания гостю о встрече (F1): `booking_id` (ON DELETE CASCADE), `owner_id`, `occurrence_start`, `kind` CHECK (`24h`/`2h`), `due_at`, `sent_at` (NULL — не отправлено); UNIQUE (booking_id, occurrence_start, kind), частичный индекс `due_at WHERE sent_at IS NULL`. Создаются при записи/переносе, неотправленные удаляются при отмене/переносе встречи; планировщик захватывает наступившие одним `UPDATE … WHERE id IN (SELECT … FOR UPDATE SKIP LOCKED) RETURNING` |

Миграция `2026_10_16_telegram_pro.sql` (то же — блоком в конце таблиц `schema.sql`):
`users.plan_until` (Pro, пока `plan_until > now()`; NULL — Free),
`users.telegram_chat_id` (уникальный частичный индекс: один чат — один аккаунт)
и `users.telegram_username`, `bookings.manage_token_enc` — токен ссылки
управления, зашифрованный AES-256-GCM ключом `SELFVOY_2FA_KEY` (AAD
`selfvoy-manage-token`; поиск записи — по-прежнему только по `manage_hash`),
таблицы `booking_telegram` и `server_state`. Опрос Telegram ведёт один экземпляр
сервера: advisory lock с отдельным ключом (`0x53564F59544C47`, не пространство
расписаний `0x4155…`) на соединении блокировок.

Миграция `2026_10_16_reminders_branding.sql` (S1b; то же — блоком в конце таблиц
`schema.sql`): `booking_pages.guest_reminders` (BOOLEAN, true — напоминать гостю),
`booking_pages.hide_branding` (BOOLEAN, false — скрыть подпись «Запись через
Selfvoy», Pro), таблица `booking_reminders`, `referrals.rewarded_at` (награда
+30 дней Pro обоим начислена; один раз). `confirmed_at` встречи («Приду») —
поле внутри `bookings.occurrences` JSONB, отдельной колонки нет. Во встроенном
хранилище — массив `booking_reminders` с теми же полями; захват — под мьютексом.

Миграция `2026_10_16_payments.sql` (S2a, F4; то же — блоком в конце таблиц
`schema.sql`): поля страницы записи `booking_pages.price` (BIGINT, минимальные
единицы валюты, 0–100 000 000), `currency` (`RUB`/`USD`/`EUR`/`KZT`/`BYN`),
`prepay` (`none`/`full`/`deposit`), `deposit`, `payment_url`, `payment_note`,
`pay_within_minutes` (15–1440, 60), `cancel_hours` (0–168, 0), `late_cancel`
(`allow`/`block`), `policy_text` — все с CHECK и значениями по умолчанию;
`bookings.payment` JSONB (NULL — оплаты нет; `{status, amount, currency, kind,
method, provider_id, action_url, due_at, paid_at, refunded_at}`, см.
PROTOCOL.md) с частичным индексом `bookings_payment_pending_idx` по
`payment->>'due_at' WHERE payment->>'status' = 'pending'` (планировщик истечения
и опроса ЮKassa); таблица `payment_accounts` — магазин ЮKassa владельца:
`user_id` PK (ON DELETE CASCADE), `provider` CHECK (`yookassa`), `shop_id`,
`secret_enc` (секретный ключ, AES-256-GCM ключом `SELFVOY_2FA_KEY`, AAD
`selfvoy-payment-secret`), `connected_at`. `bookings.cancelled_by` получает
значение `system` (оплата не поступила). Во встроенном хранилище — поля в тех же
записях и массив `payment_accounts`.

Миграция `2026_10_16_packages.sql` (S2b, F5; то же — блоком в конце таблиц
`schema.sql`): `booking_pages.packages` JSONB (`[]` по умолчанию; предложения
пакетов `[{id, title, sessions, price, valid_days}]`, не больше 5 — проверяет
сервер), таблица `package_purchases`, `bookings.cc_email` (второй получатель
извещений гостя, `''` — нет) и `bookings.package_id` (REFERENCES
`package_purchases` ON DELETE SET NULL; частичный индекс по непустым). Остаток
занятий = `sessions_total − sessions_used`; списание и возврат — под
блокировкой расписания владельца. Во встроенном хранилище — поля в тех же
записях и массив `package_purchases`.

Миграция `2026_10_17_booking_price.sql` (то же — блоком в конце таблиц
`schema.sql`): `bookings.price` INTEGER NOT NULL DEFAULT 0 (CHECK ≥ 0) и
`bookings.currency` TEXT NOT NULL DEFAULT `'RUB'` (CHECK — те же валюты, что у
страницы) — цена одной встречи на момент `booking.book` (снимок
`booking_pages.price` / `currency`; запись по пакету — 0). Смена цены страницы,
перенос и отмена снимок не меняют. Колонки добавляются в блоке `DO` только если их
ещё нет, и тогда же заполняются: записи без пакета — текущей ценой своей страницы
(точнее для старых записей не узнать), все — валютой страницы; повторный запуск
ничего не перезаписывает. Во встроенном хранилище — поля `price` / `currency` в тех
же записях (нет поля — 0 и `RUB`).

Миграция `2026_10_17_waitlist_rebook.sql` (F8/F10; то же — блоком в конце таблиц
`schema.sql`): поля страницы `booking_pages.waitlist` (BOOLEAN, false; Pro),
`waitlist_hold_minutes` (CHECK 15–240, 60), `rebook_after_days` (CHECK 0 или 7–180,
0); `bookings.rebook_sent_at` (TIMESTAMPTZ, NULL — «пора записаться снова» ещё не
обработано) и `bookings.rebook_status` (CHECK `''`/`claimed`/`sent`/`skipped`/`cancelled`)
с частичным индексом `bookings_rebook_idx (ends_at) WHERE status = 'active' AND
rebook_sent_at IS NULL` — планировщик захватывает наступившие одним `UPDATE …
WHERE id IN (SELECT … FOR UPDATE OF b SKIP LOCKED)` (ровно один раз на запись при
нескольких экземплярах); таблица `waitlist_entries`. Держание листа ожидания
(`status = 'offered'`, `offer_expires_at > now()`) — занятость владельца. Изменения
записи листа — под блокировкой расписания владельца. F10 («Мой налог»):
`user_preferences.npd_receipts` (BOOLEAN, false; `prefs.npd_receipts`); `npd`,
`receipt_task_id`, `receipt_url`, `receipt_at`, `receipt_refund_task_id` — поля
`bookings.payment` / `package_purchases.payment` (JSONB). Миграция идемпотентна
(`IF NOT EXISTS`). Во встроенном хранилище — поля в тех же записях и массив
`waitlist_entries` (захват и уникальность — под мьютексом).

Миграция `2026_10_17_telegram_admin.sql` (F7; то же — блоком в конце таблиц
`schema.sql`, идемпотентно): `booking_pages.ai_admin` BOOLEAN NOT NULL DEFAULT
false (ИИ-администратор в Telegram, Pro) и две таблицы:

| таблица | назначение |
| --- | --- |
| `telegram_guest_chats` | чат гостя, привязанный к странице командой `/start p_<slug>`: `chat_id` PK, `page_id` (ON DELETE CASCADE), `owner_id` (ON DELETE CASCADE), `guest_name` и `username` из Telegram, `history` JSONB (последние ≤ 10 сообщений `[{role: guest\|admin, text}]` — контекст ИИ), `created_at`, `updated_at`; индекс по `page_id`. `/stop` и 403 от Telegram удаляют строку |
| `telegram_questions` | вопросы гостей, переданные специалисту (`escalate`): `owner_id`, `page_id` (ON DELETE CASCADE), `chat_id`, `guest_name`, `username`, `text`, `status` CHECK (`open`/`answered`), `answer` (последний ответ `telegram.reply`), `created_at`, `answered_at`; индекс (owner_id, id DESC) |

Во встроенном хранилище — поле `ai_admin` в записи страницы и массивы
`telegram_guest_chats`, `telegram_questions` с теми же полями.

Миграция `2026_10_15_audit_fixes.sql`: `users.email_verified DEFAULT FALSE`
(новый аккаунт без явного значения не считается подтверждённым), `bookings.ends_at`
с заполнением и индексами, `rate_limits` — UNLOGGED и индекс `window_start`.

Миграция `2026_10_16_ics_feed.sql` (F6): `users.ics_token_hash` — hex sha256
токена ICS-подписки владельца (`calendar.feedReset`, `GET /ics/<токен>.ics`),
уникальный индекс `users_ics_token_hash_key`; NULL — подписка выключена. Сам
токен не хранится. Во встроенном хранилище — отдельный массив `ics_feeds`
(`user_id`, `token_hash`).

## Соглашения

- Все запросы параметризованы ($1, $2, …) — строки пользователя never
  конкатенируются в SQL (проверено e2e-сценариями SQLi, TESTING.md).
- Время — UTC, ISO-8601; идентификаторы — BIGSERIAL.
- Секреты в БД: только хэши (пароли, коды, refresh) или шифры
  (TOTP, токены интеграций, токен ссылки управления записи); ключи — из окружения, не в БД.
- Удаление аккаунта: большинство таблиц — `ON DELETE CASCADE` по
  `user_id`; осознанные `SET NULL`: `audit_logs.user_id` (журнал
  безопасности сохраняется), `messages.sender_id` (история собеседника
  не разрушается), `chats.created_by`, `tasks.chat_id`,
  `pending_actions.chat_id`. Отдельного UI-обработчика «удалить аккаунт»
  пока нет — честно указано в PRIVACY.md.
