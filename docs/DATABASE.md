# База данных Aura

PostgreSQL 14+ (прод) или встроенное JSON-хранилище (разработка/тесты,
`AURA_EMBEDDED_DB`). Оба бэкенда реализуют один интерфейс `IDatabase`
(`server/include/aura/idatabase.h`); C++-тесты работают на встроенном,
e2e — на реальном PostgreSQL.

Схема: `schema/schema.sql` (идемпотентна, `CREATE … IF NOT EXISTS`);
изменения — файлами `schema/migrations/` (применяются по порядку имени).
Демо-данные: `schema/seed.sql`.

## 22 таблицы по доменам

### Аутентификация и безопасность
| таблица | назначение |
| --- | --- |
| `users` | аккаунты: email (уникальный, нормализованный), хэш пароля Argon2id, `email_verified`, `active` |
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
| `user_preferences` | JSONB-настройки (тема, диета, город, бюджет, `notifications.quiet_hours`, `notifications.muted_kinds`) |

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
| `integration_oauth_states` | одноразовые state + PKCE verifier (TTL `AURA_OAUTH_STATE_TTL`) |

### Уведомления (этап 13)
| таблица | назначение |
| --- | --- |
| `notifications` | in-app «входящая»: kind, заголовок, тело, payload JSONB, `read_at`; индексы (user_id, id DESC) и частичный по непрочитанным |
| `push_devices` | токены push-доставки: `platform` CHECK (`apns`/`webhook`/`dev`), UNIQUE(user_id, token), `enabled`, `last_used_at` |

## Соглашения

- Все запросы параметризованы ($1, $2, …) — строки пользователя never
  конкатенируются в SQL (проверено e2e-сценариями SQLi, TESTING.md).
- Время — UTC, ISO-8601; идентификаторы — BIGSERIAL.
- Секреты в БД: только хэши (пароли, коды, refresh) или шифры
  (TOTP, токены интеграций); ключи — из окружения, не в БД.
- Удаление аккаунта: большинство таблиц — `ON DELETE CASCADE` по
  `user_id`; осознанные `SET NULL`: `audit_logs.user_id` (журнал
  безопасности сохраняется), `messages.sender_id` (история собеседника
  не разрушается), `chats.created_by`, `tasks.chat_id`,
  `pending_actions.chat_id`. Отдельного UI-обработчика «удалить аккаунт»
  пока нет — честно указано в PRIVACY.md.
