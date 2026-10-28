-- ============================================================================
--  Selfvoy — схема PostgreSQL
--  Таблицы ровно по скелету проекта:
--    Users, Chats, ChatMembers, Messages, UserMemory, UserPreferences, Sessions
--
--  Применение:  psql "$SELFVOY_DATABASE_URL" -f schema/schema.sql
-- ============================================================================

-- Требуется PostgreSQL 13+ (gen_random_uuid() входит в ядро, pgcrypto не нужен).

BEGIN;

-- ---------------------------------------------------------------- Users ----
CREATE TABLE IF NOT EXISTS users (
    id             BIGSERIAL     PRIMARY KEY,
    email          TEXT          NOT NULL,
    password_hash  TEXT          NOT NULL,              -- pbkdf2$<iters>$<salt>$<hash>
    display_name   TEXT          NOT NULL,
    avatar_url     TEXT          NOT NULL DEFAULT '',
    bio            TEXT          NOT NULL DEFAULT '',
    timezone       TEXT          NOT NULL DEFAULT 'Europe/Moscow',
    is_active      BOOLEAN       NOT NULL DEFAULT TRUE,
    email_verified BOOLEAN       NOT NULL DEFAULT FALSE, -- подтверждён ли email (fail closed)
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_seen_at   TIMESTAMPTZ,
    -- админ-панель (admin.*); первые админы — SELFVOY_ADMIN_EMAILS
    role           TEXT          NOT NULL DEFAULT 'user'
                   CONSTRAINT users_role_check CHECK (role IN ('user', 'moderator', 'admin'))
);

-- Email уникален без учёта регистра (citext не требуется)
CREATE UNIQUE INDEX IF NOT EXISTS users_email_lower_key ON users (lower(email));
CREATE INDEX IF NOT EXISTS users_role_idx ON users (role) WHERE role <> 'user';
-- ICS-подписка владельца (calendar.feed*, GET /ics/<токен>.ics): hex sha256 токена
-- ссылки, сам токен не хранится; NULL — подписка выключена. Миграция 2026_10_16_ics_feed.sql.
ALTER TABLE users ADD COLUMN IF NOT EXISTS ics_token_hash TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS users_ics_token_hash_key ON users (ics_token_hash);

-- ------------------------------------------------------------- Sessions ----
-- Один активный WebSocket/JWT-сеанс на устройство.
CREATE TABLE IF NOT EXISTS sessions (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    jwt_id         TEXT          NOT NULL,              -- jti из токена
    device         TEXT          NOT NULL DEFAULT 'qt-client',
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    revoked_at     TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS sessions_user_idx  ON sessions (user_id);
CREATE INDEX IF NOT EXISTS sessions_jwtid_idx ON sessions (jwt_id);

-- ------------------------------------------------------------ AuthTokens ----
-- Одноразовые коды: подтверждение email и сброс пароля.
-- Храним только SHA-256 от кода — утёкшая БД не раскрывает сами коды.
CREATE TABLE IF NOT EXISTS auth_tokens (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose        TEXT          NOT NULL CHECK (purpose IN ('email_verify', 'password_reset')),
    token_hash     TEXT          NOT NULL,
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    used_at        TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS auth_tokens_lookup_idx ON auth_tokens (purpose, token_hash);
CREATE INDEX IF NOT EXISTS auth_tokens_user_idx   ON auth_tokens (user_id, purpose);

-- ---------------------------------------------------------- RefreshTokens ----
-- Refresh-токены с ротацией по «семействам»: повторное использование
-- отозванного токена отзывает всё семейство (защита от украденного токена).
CREATE TABLE IF NOT EXISTS refresh_tokens (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    family_id      UUID          NOT NULL,
    token_hash     TEXT          NOT NULL UNIQUE,        -- sha256(токена)
    device         TEXT          NOT NULL DEFAULT '',
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    revoked_at     TIMESTAMPTZ,
    replaced_by    UUID                                -- токен, выданный при ротации
);

CREATE INDEX IF NOT EXISTS refresh_tokens_family_idx ON refresh_tokens (family_id);
CREATE INDEX IF NOT EXISTS refresh_tokens_user_idx   ON refresh_tokens (user_id);

-- ------------------------------------------------------------- AuditLogs ----
-- Журнал событий безопасности (входы, сбросы пароля, отзывы сессий, 2FA).
CREATE TABLE IF NOT EXISTS audit_logs (
    id             BIGSERIAL     PRIMARY KEY,
    user_id        BIGINT        REFERENCES users(id) ON DELETE SET NULL,
    kind           TEXT          NOT NULL,
    detail         JSONB         NOT NULL DEFAULT '{}'::jsonb,
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS audit_logs_user_idx ON audit_logs (user_id, id DESC);

-- ------------------------------------------------------- TwoFactorSettings --
-- Настройка TOTP 2FA. Секрет хранится зашифрованным (AES-256-GCM, ключ — env
-- SELFVOY_2FA_KEY); enabled=true только после подтверждения одноразовым кодом.
CREATE TABLE IF NOT EXISTS two_factor_settings (
    user_id           BIGINT        PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    secret_encrypted  TEXT          NOT NULL,           -- AES-256-GCM blob, base64
    enabled           BOOLEAN       NOT NULL DEFAULT FALSE,
    last_used_counter BIGINT        NOT NULL DEFAULT 0, -- защита от повторного использования кода
    created_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    enabled_at        TIMESTAMPTZ,
    updated_at        TIMESTAMPTZ   NOT NULL DEFAULT now()
);

-- ---------------------------------------------------------- RecoveryCodes --
-- Резервные коды восстановления: храним только SHA-256(code), код одноразовый.
CREATE TABLE IF NOT EXISTS recovery_codes (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    code_hash      TEXT          NOT NULL,             -- sha256(кода)
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    used_at        TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS recovery_codes_user_idx ON recovery_codes (user_id, code_hash);

-- --------------------------------------------------------- TrustedDevices --
-- Доверенные устройства: после подтверждения 2FA освобождают устройство от
-- повторного ввода кода на 90 дней (или до отзыва).
CREATE TABLE IF NOT EXISTS trusted_devices (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_hash    TEXT          NOT NULL,             -- sha256(device_id)
    device         TEXT          NOT NULL DEFAULT '',
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    last_used_at   TIMESTAMPTZ,
    revoked_at     TIMESTAMPTZ,
    UNIQUE (user_id, device_hash)
);

CREATE INDEX IF NOT EXISTS trusted_devices_user_idx ON trusted_devices (user_id);

-- ---------------------------------------------------------------- Chats ----
CREATE TABLE IF NOT EXISTS chats (
    id               BIGSERIAL   PRIMARY KEY,
    kind             TEXT        NOT NULL DEFAULT 'direct'
                     CHECK (kind IN ('direct', 'group', 'agent')),
    title            TEXT        NOT NULL DEFAULT '',
    created_by       BIGINT      REFERENCES users(id) ON DELETE SET NULL,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_message_at  TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS chats_last_message_idx ON chats (last_message_at DESC NULLS LAST);

-- --------------------------------------------------------- ChatMembers ----
CREATE TABLE IF NOT EXISTS chat_members (
    chat_id         BIGINT       NOT NULL REFERENCES chats(id)  ON DELETE CASCADE,
    user_id         BIGINT       NOT NULL REFERENCES users(id)  ON DELETE CASCADE,
    role            TEXT         NOT NULL DEFAULT 'member'
                    CHECK (role IN ('member', 'owner', 'agent')),
    joined_at       TIMESTAMPTZ  NOT NULL DEFAULT now(),
    last_read_at    TIMESTAMPTZ,
    is_muted        BOOLEAN      NOT NULL DEFAULT FALSE,
    PRIMARY KEY (chat_id, user_id)
);

CREATE INDEX IF NOT EXISTS chat_members_user_idx ON chat_members (user_id);

-- ------------------------------------------------------------- Messages ----
CREATE TABLE IF NOT EXISTS messages (
    id             BIGSERIAL     PRIMARY KEY,
    chat_id        BIGINT        NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
    sender_id      BIGINT        REFERENCES users(id) ON DELETE SET NULL,   -- NULL = системное
    kind           TEXT          NOT NULL DEFAULT 'text'
                   CHECK (kind IN ('text', 'agent_action', 'agent_reply', 'system')),
    body           TEXT          NOT NULL DEFAULT '',
    payload        JSONB         NOT NULL DEFAULT '{}'::jsonb,  -- действия Selfvoy, A2A-предложения
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    edited_at      TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS messages_chat_created_idx ON messages (chat_id, created_at DESC);
CREATE INDEX IF NOT EXISTS messages_payload_gin_idx  ON messages USING gin (payload);

-- ----------------------------------------------------------- UserMemory ----
-- Долговременная память: факты, предпочтения, расписание.
CREATE TABLE IF NOT EXISTS user_memory (
    id             BIGSERIAL     PRIMARY KEY,
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind           TEXT          NOT NULL DEFAULT 'fact'
                   CHECK (kind IN ('fact', 'preference', 'schedule', 'contact')),
    text           TEXT          NOT NULL,
    weight         REAL          NOT NULL DEFAULT 1.0,
    tags           TEXT[]        NOT NULL DEFAULT '{}',
    updated_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    UNIQUE (user_id, text)
);

CREATE INDEX IF NOT EXISTS user_memory_user_idx ON user_memory (user_id, weight DESC);
CREATE INDEX IF NOT EXISTS user_memory_tags_idx ON user_memory USING gin (tags);

-- ---------------------------------------------------- UserPreferences ----
CREATE TABLE IF NOT EXISTS user_preferences (
    user_id          BIGINT      PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    diet             TEXT[]      NOT NULL DEFAULT '{}',
    transport        TEXT        NOT NULL DEFAULT 'walk',
    preferred_hours  INTEGER[]   NOT NULL DEFAULT '{10,11,12,16,17,18,19}',
    work_hours       INTEGER[]   NOT NULL DEFAULT '{9,10,11,12,13,14,15,16,17,18}',
    budget_limit     NUMERIC(10,2) NOT NULL DEFAULT 0,
    city             TEXT        NOT NULL DEFAULT '',
    lat              DOUBLE PRECISION,
    lon              DOUBLE PRECISION,
    theme            TEXT        NOT NULL DEFAULT 'graphite'
                     CHECK (theme IN ('graphite', 'graphite-light')),
    notifications    JSONB       NOT NULL DEFAULT '{"push": true, "email": false}'::jsonb,
    -- Онбординг-опрос после регистрации (этап 8+): день рождения и аллергии
    -- попадают в контекст AI (MemoryManager.context → preferences).
    birthday         TEXT        NOT NULL DEFAULT '',      -- YYYY-MM-DD
    allergies        TEXT[]      NOT NULL DEFAULT '{}',
    onboarded        BOOLEAN     NOT NULL DEFAULT FALSE,   -- опрос пройден
    -- Приватность Selfvoy в переговорах с чужими агентами Selfvoy (сервер применяет при A2A).
    privacy          JSONB       NOT NULL DEFAULT
                     '{"share_free_slots_only": true, "hide_event_titles": true, "allow_place_choice": true}'::jsonb,
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ------------------------------------------------------ ToolPermissions ----
-- Разрешения пользователя на инструменты Selfvoy (этап 8, ядро безопасности).
-- Правило v3: LLM лишь формирует намерение, исполняет сервер и только после
-- проверки прав. mode:
--   allow — исполнять сразу;
--   ask   — создать отложенное действие и ждать подтверждения пользователя;
--   deny  — не исполнять.
-- Если строки нет, режим выводится из опасности инструмента (опасные → ask).
CREATE TABLE IF NOT EXISTS tool_permissions (
    user_id     BIGINT      NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    tool        TEXT        NOT NULL,
    mode        TEXT        NOT NULL DEFAULT 'ask'
                CHECK (mode IN ('allow', 'ask', 'deny')),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, tool)
);

-- ------------------------------------------------------- PendingActions ----
-- Действия, требующие явного подтверждения (барьер подтверждения). Selfvoy
-- создаёт запись вместо исполнения; выполнение происходит только после
-- confirmation.approve. Храним намерение (tool+args) и итог (result).
CREATE TABLE IF NOT EXISTS pending_actions (
    id           BIGSERIAL     PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    chat_id      BIGINT        REFERENCES chats(id) ON DELETE SET NULL,
    tool         TEXT          NOT NULL,
    args         JSONB         NOT NULL DEFAULT '{}'::jsonb,
    summary      TEXT          NOT NULL DEFAULT '',       -- человекочитаемое описание
    status       TEXT          NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending', 'approved', 'denied', 'executed', 'failed', 'expired')),
    result       JSONB         NOT NULL DEFAULT '{}'::jsonb,
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    resolved_at  TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS pending_actions_user_idx ON pending_actions (user_id, status, id DESC);

-- ----------------------------------------------------------------- Tasks ----
-- Задачи и напоминания (этап 8). remind_at — когда планировщик должен
-- прислать уведомление; reminded_at отмечается после отправки (одноразово).
CREATE TABLE IF NOT EXISTS tasks (
    id           BIGSERIAL     PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    chat_id      BIGINT        REFERENCES chats(id) ON DELETE SET NULL,
    title        TEXT          NOT NULL,
    notes        TEXT          NOT NULL DEFAULT '',
    status       TEXT          NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending', 'done', 'cancelled')),
    priority     INT           NOT NULL DEFAULT 0,
    due_at       TIMESTAMPTZ,
    remind_at    TIMESTAMPTZ,
    reminded_at  TIMESTAMPTZ,
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    completed_at TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS tasks_user_idx ON tasks (user_id, status, id DESC);
CREATE INDEX IF NOT EXISTS tasks_due_idx ON tasks (remind_at)
    WHERE status = 'pending' AND reminded_at IS NULL;

-- ---------------------------------------------- IntegrationConnections ----
-- Подключённые внешние сервисы (этап 9): Google Calendar, Gmail.
-- Токены хранятся ТОЛЬКО зашифрованными (AES-256-GCM, ключ — env SELFVOY_2FA_KEY),
-- наружу (в клиентах/логах) не отдаются никогда.
CREATE TABLE IF NOT EXISTS integration_connections (
    id              BIGSERIAL     PRIMARY KEY,
    user_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider        TEXT          NOT NULL,           -- google_calendar | google_gmail
    account         TEXT          NOT NULL DEFAULT '', -- email аккаунта (для показа)
    scope           TEXT          NOT NULL DEFAULT '',
    token_encrypted TEXT          NOT NULL,            -- AES-256-GCM {access,refresh,expires_at}
    status          TEXT          NOT NULL DEFAULT 'active'
                    CHECK (status IN ('active', 'expired', 'revoked', 'error')),
    last_error      TEXT          NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_used_at    TIMESTAMPTZ,
    UNIQUE (user_id, provider)
);

CREATE INDEX IF NOT EXISTS integration_connections_user_idx ON integration_connections (user_id);

-- ----------------------------------------------- IntegrationOauthStates ----
-- Одноразовые состояния OAuth 2.0 + PKCE: state защищает от CSRF,
-- code_verifier нужен для обмена кода на токены. Срок жизни — минуты.
CREATE TABLE IF NOT EXISTS integration_oauth_states (
    state        TEXT          PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider     TEXT          NOT NULL,
    verifier     TEXT          NOT NULL,              -- PKCE code_verifier
    redirect_uri TEXT          NOT NULL DEFAULT '',
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ   NOT NULL,
    used_at      TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS integration_oauth_states_user_idx ON integration_oauth_states (user_id);

-- In-app уведомления (этап 13): единая «входящая» для событий, которые
-- пользователь мог пропустить (напоминание, подтверждение, A2A-предложение,
-- новый вход, изменение 2FA).
CREATE TABLE IF NOT EXISTS notifications (
    id         BIGSERIAL     PRIMARY KEY,
    user_id    BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind       TEXT          NOT NULL,
    title      TEXT          NOT NULL,
    body       TEXT          NOT NULL DEFAULT '',
    payload    JSONB         NOT NULL DEFAULT '{}'::jsonb,
    read_at    TIMESTAMPTZ,
    created_at TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS notifications_user_idx ON notifications (user_id, id DESC);
CREATE INDEX IF NOT EXISTS notifications_unread_idx ON notifications (user_id)
    WHERE read_at IS NULL;

-- Устройства для push (этап 13): APNs-токены iOS. Доставка — через драйвер
-- (dev|webhook); реальный APNs терминирует push-шлюз за TLS-прокси.
CREATE TABLE IF NOT EXISTS push_devices (
    id           BIGSERIAL     PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    platform     TEXT          NOT NULL DEFAULT 'apns'
                 CHECK (platform IN ('apns', 'fcm', 'webhook', 'dev')),
    token        TEXT          NOT NULL,
    enabled      BOOLEAN       NOT NULL DEFAULT TRUE,
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_used_at TIMESTAMPTZ,
    UNIQUE (user_id, token)
);

CREATE INDEX IF NOT EXISTS push_devices_user_idx ON push_devices (user_id);

-- Приглашения по ссылке: владелец договаривается о встрече с человеком без Selfvoy.
-- В БД только sha256 токена (сама ссылка показывается владельцу один раз).
-- slots — предложенные интервалы [{start,end}] (ISO UTC); chosen — выбранный гостем.
-- Статус expired не хранится: это open с истёкшим expires_at.
CREATE TABLE IF NOT EXISTS meeting_invites (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash       TEXT          NOT NULL UNIQUE,
    topic            TEXT          NOT NULL,
    note             TEXT          NOT NULL DEFAULT '',
    duration_minutes INTEGER       NOT NULL CHECK (duration_minutes BETWEEN 15 AND 240),
    slots            JSONB         NOT NULL DEFAULT '[]'::jsonb,
    status           TEXT          NOT NULL DEFAULT 'open'
                     CHECK (status IN ('open', 'accepted', 'declined', 'cancelled')),
    guest_name       TEXT          NOT NULL DEFAULT '',
    guest_email      TEXT          NOT NULL DEFAULT '',
    guest_comment    TEXT          NOT NULL DEFAULT '',
    chosen           JSONB         NOT NULL DEFAULT '{}'::jsonb,
    expires_at       TIMESTAMPTZ   NOT NULL,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    responded_at     TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS meeting_invites_owner_idx ON meeting_invites (owner_id, id DESC);

-- Страницы записи (booking.*): постоянная публичная ссылка владельца (#/b/<slug>),
-- гости без Selfvoy сами записываются на свободное время. См. docs/PROTOCOL.md.
CREATE TABLE IF NOT EXISTS booking_pages (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    slug             TEXT          NOT NULL UNIQUE,
    title            TEXT          NOT NULL,
    description      TEXT          NOT NULL DEFAULT '',
    duration_minutes INTEGER       NOT NULL CHECK (duration_minutes BETWEEN 15 AND 240),
    days_ahead       INTEGER       NOT NULL DEFAULT 14 CHECK (days_ahead BETWEEN 1 AND 60),
    max_per_day      INTEGER       NOT NULL DEFAULT 4 CHECK (max_per_day BETWEEN 1 AND 16),
    notice_hours     INTEGER       NOT NULL DEFAULT 2 CHECK (notice_hours BETWEEN 0 AND 168),
    max_series       INTEGER       NOT NULL DEFAULT 1 CHECK (max_series BETWEEN 1 AND 12),
    weekends         BOOLEAN       NOT NULL DEFAULT false,
    status           TEXT          NOT NULL DEFAULT 'active'
                     CHECK (status IN ('active', 'paused', 'archived')),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS booking_pages_owner_idx ON booking_pages (owner_id, id DESC);

-- Записи гостей. occurrences — встречи (одна или еженедельная серия):
-- [{start, end, day, status: active|cancelled, task_id}]. В БД только sha256
-- токена ссылки управления (#/m/<токен>).
CREATE TABLE IF NOT EXISTS bookings (
    id               BIGSERIAL     PRIMARY KEY,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    manage_hash      TEXT          NOT NULL UNIQUE,
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL DEFAULT '',
    guest_comment    TEXT          NOT NULL DEFAULT '',
    occurrences      JSONB         NOT NULL DEFAULT '[]'::jsonb,
    status           TEXT          NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'cancelled')),
    cancelled_by     TEXT          NOT NULL DEFAULT '',
    reschedules      INTEGER       NOT NULL DEFAULT 0,
    ends_at          TIMESTAMPTZ,  -- конец последней активной встречи (NULL — активных нет)
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS bookings_owner_idx ON bookings (owner_id, id DESC);
CREATE INDEX IF NOT EXISTS bookings_page_idx ON bookings (page_id, status);
-- Занятость владельца и лимит в день читают только записи с предстоящими встречами.
CREATE INDEX IF NOT EXISTS bookings_owner_upcoming_idx ON bookings (owner_id, ends_at) WHERE status = 'active';
CREATE INDEX IF NOT EXISTS bookings_page_upcoming_idx ON bookings (page_id, ends_at) WHERE status = 'active';

-- Рефералы (referral.*): у каждого пользователя один код; приглашённый
-- записывается при регистрации (auth.register → ref), verified_at — после
-- подтверждения email.
CREATE TABLE IF NOT EXISTS referral_codes (
    user_id          BIGINT        PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    code             TEXT          NOT NULL UNIQUE CHECK (code ~ '^[A-Z0-9]{8}$'),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS referrals (
    id               BIGSERIAL     PRIMARY KEY,
    referrer_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    referred_id      BIGINT        NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
    source           TEXT          NOT NULL DEFAULT 'link' CHECK (source IN ('link', 'booking', 'invite')),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    verified_at      TIMESTAMPTZ,
    CHECK (referrer_id <> referred_id)
);

CREATE INDEX IF NOT EXISTS referrals_referrer_idx ON referrals (referrer_id, id DESC);

-- Счётчики попыток, общие для всех экземпляров сервера (вход, коды, 2FA, лимиты
-- гостевых методов): окно [window_start, window_start + window_sec). Истёкшие
-- строки сервер периодически удаляет.
-- UNLOGGED: счётчики одноразовые — без WAL каждая попытка не ждёт fsync
-- (после аварийного рестарта PostgreSQL таблица пуста — это допустимо).
CREATE UNLOGGED TABLE IF NOT EXISTS rate_limits (
    key              TEXT          PRIMARY KEY,
    count            INTEGER       NOT NULL,
    window_start     TIMESTAMPTZ   NOT NULL,
    window_sec       INTEGER       NOT NULL CHECK (window_sec > 0)
);
CREATE INDEX IF NOT EXISTS rate_limits_window_idx ON rate_limits (window_start);

-- ------------------------------------------- Рост (S1a): тариф, Telegram ---
-- Те же изменения для существующей базы — миграция 2026_10_16_telegram_pro.sql.
-- Тариф: Pro действует, пока plan_until > now() (NULL — Free).
ALTER TABLE users ADD COLUMN IF NOT EXISTS plan_until TIMESTAMPTZ;
-- Telegram владельца (уведомления о записях): один чат — один аккаунт.
ALTER TABLE users ADD COLUMN IF NOT EXISTS telegram_chat_id BIGINT;
ALTER TABLE users ADD COLUMN IF NOT EXISTS telegram_username TEXT NOT NULL DEFAULT '';
CREATE UNIQUE INDEX IF NOT EXISTS users_telegram_chat_key ON users (telegram_chat_id)
    WHERE telegram_chat_id IS NOT NULL;

-- Токен ссылки управления #/m/<токен>, зашифрованный AES-256-GCM (ключ — как у
-- TOTP-секретов, AAD "selfvoy-manage-token"): напоминания дают гостю ссылку.
-- Поиск — по-прежнему только по manage_hash. NULL — запись до этой версии.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS manage_token_enc TEXT;

-- Telegram-чаты гостя записи (напоминания): не больше 3 на запись (проверяет сервер).
CREATE TABLE IF NOT EXISTS booking_telegram (
    booking_id       BIGINT        NOT NULL REFERENCES bookings(id) ON DELETE CASCADE,
    chat_id          BIGINT        NOT NULL,
    linked_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    PRIMARY KEY (booking_id, chat_id)
);
CREATE INDEX IF NOT EXISTS booking_telegram_chat_idx ON booking_telegram (chat_id);

-- Служебное состояние сервера (смещение getUpdates Telegram и т.п.).
CREATE TABLE IF NOT EXISTS server_state (
    key              TEXT          PRIMARY KEY,
    value            TEXT          NOT NULL,
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

-- ------------------- Рост (S1b): напоминания гостю, подпись, награда ---
-- Те же изменения для существующей базы — миграция 2026_10_16_reminders_branding.sql.
-- Поля страницы записи: напоминания гостю (бесплатно) и скрытие подписи (Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS guest_reminders BOOLEAN NOT NULL DEFAULT true;
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS hide_branding BOOLEAN NOT NULL DEFAULT false;

-- Напоминания гостю за сутки (24h) и за 2 часа (2h) до каждой активной встречи.
-- Создаются при записи и переносе, неотправленные удаляются при отмене/переносе
-- встречи; sent_at ставит атомарный захват планировщика (FOR UPDATE SKIP LOCKED).
CREATE TABLE IF NOT EXISTS booking_reminders (
    id               BIGSERIAL     PRIMARY KEY,
    booking_id       BIGINT        REFERENCES bookings(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL,
    occurrence_start TIMESTAMPTZ   NOT NULL,
    kind             TEXT          NOT NULL CHECK (kind IN ('24h', '2h')),
    due_at           TIMESTAMPTZ   NOT NULL,
    sent_at          TIMESTAMPTZ,
    UNIQUE (booking_id, occurrence_start, kind)
);
CREATE INDEX IF NOT EXISTS booking_reminders_due_idx ON booking_reminders (due_at) WHERE sent_at IS NULL;

-- Награда за приглашённого (+30 дней Pro обоим) начисляется один раз.
ALTER TABLE referrals ADD COLUMN IF NOT EXISTS rewarded_at TIMESTAMPTZ;

-- ------------------------------ Рост (S2a): цена, предоплата, политика отмены ---
-- Те же изменения для существующей базы — миграция 2026_10_16_payments.sql.
-- Поля страницы записи (F4): цена (мин. единицы валюты), предоплата и политика отмены.
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS price BIGINT NOT NULL DEFAULT 0
    CHECK (price BETWEEN 0 AND 100000000);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS currency TEXT NOT NULL DEFAULT 'RUB'
    CHECK (currency IN ('RUB', 'USD', 'EUR', 'KZT', 'BYN'));
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS prepay TEXT NOT NULL DEFAULT 'none'
    CHECK (prepay IN ('none', 'full', 'deposit'));
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS deposit BIGINT NOT NULL DEFAULT 0 CHECK (deposit >= 0);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS payment_url TEXT NOT NULL DEFAULT '';
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS payment_note TEXT NOT NULL DEFAULT '';
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS pay_within_minutes INTEGER NOT NULL DEFAULT 60
    CHECK (pay_within_minutes BETWEEN 15 AND 1440);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS cancel_hours INTEGER NOT NULL DEFAULT 0
    CHECK (cancel_hours BETWEEN 0 AND 168);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS late_cancel TEXT NOT NULL DEFAULT 'allow'
    CHECK (late_cancel IN ('allow', 'block'));
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS policy_text TEXT NOT NULL DEFAULT '';

-- Оплата записи: {status, amount, currency, kind, method, provider_id, action_url,
-- due_at, paid_at, refunded_at} (см. docs/PROTOCOL.md); NULL — оплаты нет.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS payment JSONB;
-- Планировщик ищет ожидающие оплаты (pending) по сроку due_at.
CREATE INDEX IF NOT EXISTS bookings_payment_pending_idx ON bookings ((payment->>'due_at'))
    WHERE payment->>'status' = 'pending';

-- Магазин ЮKassa владельца: деньги гостей идут ему напрямую. secret_enc —
-- секретный ключ, AES-256-GCM (AAD "selfvoy-payment-secret").
CREATE TABLE IF NOT EXISTS payment_accounts (
    user_id          BIGINT        PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    provider         TEXT          NOT NULL CHECK (provider IN ('yookassa')),
    shop_id          TEXT          NOT NULL,
    secret_enc       TEXT          NOT NULL,
    connected_at     TIMESTAMPTZ   NOT NULL DEFAULT now()
);

-- ----------------- Рост (S2b): пакеты/абонементы, второй получатель ---
-- Те же изменения для существующей базы — миграция 2026_10_16_packages.sql.
-- Предложения пакетов страницы записи: [{id, title, sessions, price, valid_days}], не больше 5.
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS packages JSONB NOT NULL DEFAULT '[]'::jsonb;

-- Покупки пакетов: снимок предложения (название, занятия, цена, срок) на момент покупки
-- или выдачи владельцем. В БД — sha256 токена ссылки #/p/<токен> и сам токен,
-- зашифрованный AES-256-GCM (AAD "selfvoy-package-token"), — для писем гостю.
CREATE TABLE IF NOT EXISTS package_purchases (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    offer_id         INTEGER       NOT NULL DEFAULT 0,
    title            TEXT          NOT NULL,
    sessions_total   INTEGER       NOT NULL CHECK (sessions_total BETWEEN 1 AND 1000),
    sessions_used    INTEGER       NOT NULL DEFAULT 0 CHECK (sessions_used >= 0),
    valid_days       INTEGER       NOT NULL DEFAULT 90 CHECK (valid_days BETWEEN 1 AND 3650),
    price            BIGINT        NOT NULL DEFAULT 0 CHECK (price BETWEEN 0 AND 100000000),
    currency         TEXT          NOT NULL DEFAULT 'RUB' CHECK (currency IN ('RUB', 'USD', 'EUR', 'KZT', 'BYN')),
    valid_until      TIMESTAMPTZ,  -- NULL — ещё не активирован (ждёт оплату)
    status           TEXT          NOT NULL DEFAULT 'pending'
                     CHECK (status IN ('pending', 'active', 'used', 'expired', 'cancelled')),
    payment          JSONB,        -- оплата (как bookings.payment, kind = package); NULL — без оплаты
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL DEFAULT '',
    cc_email         TEXT          NOT NULL DEFAULT '',
    token_hash       TEXT          NOT NULL UNIQUE,
    token_enc        TEXT,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS package_purchases_owner_idx ON package_purchases (owner_id, id DESC);
-- Планировщик: ожидающие оплату (по сроку) и истекающие активные пакеты.
CREATE INDEX IF NOT EXISTS package_purchases_pending_idx ON package_purchases ((payment->>'due_at'))
    WHERE status = 'pending';
CREATE INDEX IF NOT EXISTS package_purchases_valid_idx ON package_purchases (valid_until) WHERE status = 'active';

-- Записи: второй получатель извещений гостя (копия, например родителю) и пакет записи.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS cc_email TEXT NOT NULL DEFAULT '';
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS package_id BIGINT REFERENCES package_purchases(id) ON DELETE SET NULL;
CREATE INDEX IF NOT EXISTS bookings_package_idx ON bookings (package_id) WHERE package_id IS NOT NULL;

-- Снимок цены записи (миграция 2026_10_17_booking_price.sql): цена одной встречи на
-- момент записи (мин. единицы; 0 — по пакету или бесплатно) и её валюта. Колонки
-- добавляются и заполняются (текущей ценой страницы, кроме записей по пакету) один
-- раз — повторный запуск схемы снимки не трогает.
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                   WHERE table_schema = current_schema() AND table_name = 'bookings'
                     AND column_name = 'currency') THEN
        ALTER TABLE bookings ADD COLUMN currency TEXT NOT NULL DEFAULT 'RUB'
            CHECK (currency IN ('RUB', 'USD', 'EUR', 'KZT', 'BYN'));
        UPDATE bookings b SET currency = p.currency FROM booking_pages p WHERE p.id = b.page_id;
    END IF;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                   WHERE table_schema = current_schema() AND table_name = 'bookings'
                     AND column_name = 'price') THEN
        ALTER TABLE bookings ADD COLUMN price INTEGER NOT NULL DEFAULT 0 CHECK (price >= 0);
        UPDATE bookings b SET price = p.price FROM booking_pages p
            WHERE p.id = b.page_id AND b.package_id IS NULL;
    END IF;
END
$$;

-- ------- Рост, этап 2: лист ожидания, повторный приём (F8), «Мой налог» (F10) ---
-- Те же изменения для существующей базы — миграция 2026_10_17_waitlist_rebook.sql.
-- Поля страницы записи: лист ожидания (Pro), сколько минут освободившееся время
-- держится за человеком из листа, через сколько дней после последней встречи
-- напомнить гостю записаться снова (0 — выкл., иначе 7–180; Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS waitlist BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS waitlist_hold_minutes INTEGER NOT NULL DEFAULT 60
    CHECK (waitlist_hold_minutes BETWEEN 15 AND 240);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS rebook_after_days INTEGER NOT NULL DEFAULT 0
    CHECK (rebook_after_days = 0 OR rebook_after_days BETWEEN 7 AND 180);

-- «Пора записаться снова»: когда планировщик обработал запись (NULL — ещё нет) и итог.
-- Ровно один раз на запись: захват — UPDATE … FOR UPDATE SKIP LOCKED.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS rebook_sent_at TIMESTAMPTZ;
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS rebook_status TEXT NOT NULL DEFAULT ''
    CHECK (rebook_status IN ('', 'claimed', 'sent', 'skipped', 'cancelled'));
CREATE INDEX IF NOT EXISTS bookings_rebook_idx ON bookings (ends_at)
    WHERE status = 'active' AND rebook_sent_at IS NULL;

-- Лист ожидания страницы записи. Освободившееся время держится за самой старой
-- подходящей записью (offer_*, до offer_expires_at); гость забирает его по ссылке
-- #/w/<токен>: в БД — sha256 токена и сам токен, зашифрованный AES-256-GCM
-- (AAD "selfvoy-waitlist-token"), — для писем и бота.
CREATE TABLE IF NOT EXISTS waitlist_entries (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL,
    days             JSONB         NOT NULL DEFAULT '[]'::jsonb,  -- местные даты владельца; [] — любой день
    part_of_day      TEXT          NOT NULL DEFAULT 'any'
                     CHECK (part_of_day IN ('any', 'morning', 'afternoon', 'evening')),
    comment          TEXT          NOT NULL DEFAULT '',
    status           TEXT          NOT NULL DEFAULT 'waiting'
                     CHECK (status IN ('waiting', 'offered', 'booked', 'left', 'expired')),
    token_hash       TEXT          NOT NULL UNIQUE,
    token_enc        TEXT,
    telegram_chat_id BIGINT,       -- подписка на предложения в боте; NULL — нет
    offer_start      TIMESTAMPTZ,  -- предложенное (держащееся) время; NULL — предложения не было
    offer_end        TIMESTAMPTZ,
    offer_day        TEXT          NOT NULL DEFAULT '',
    offer_local      TEXT          NOT NULL DEFAULT '',
    offer_expires_at TIMESTAMPTZ,
    offer_status     TEXT          NOT NULL DEFAULT ''
                     CHECK (offer_status IN ('', 'offered', 'claimed', 'expired', 'cancelled')),
    booking_id       BIGINT        REFERENCES bookings(id) ON DELETE SET NULL,
    queued_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at       TIMESTAMPTZ   NOT NULL,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
-- Один email — одна активная запись листа на страницу.
CREATE UNIQUE INDEX IF NOT EXISTS waitlist_entries_active_email_key ON waitlist_entries (page_id, lower(guest_email))
    WHERE status IN ('waiting', 'offered');
CREATE INDEX IF NOT EXISTS waitlist_entries_owner_idx ON waitlist_entries (owner_id, id DESC);
CREATE INDEX IF NOT EXISTS waitlist_entries_queue_idx ON waitlist_entries (page_id, queued_at) WHERE status = 'waiting';
-- Держания (занятость владельца) и планировщик: истекающие предложения и записи.
CREATE INDEX IF NOT EXISTS waitlist_entries_holds_idx ON waitlist_entries (owner_id, offer_expires_at)
    WHERE status = 'offered';
CREATE INDEX IF NOT EXISTS waitlist_entries_offer_due_idx ON waitlist_entries (offer_expires_at)
    WHERE status = 'offered';
CREATE INDEX IF NOT EXISTS waitlist_entries_expiry_idx ON waitlist_entries (expires_at) WHERE status = 'waiting';
CREATE INDEX IF NOT EXISTS waitlist_entries_telegram_idx ON waitlist_entries (telegram_chat_id)
    WHERE telegram_chat_id IS NOT NULL;

-- F10 («Мой налог»): настройка владельца «я самозанятый, напоминать выдать чек» (Pro).
-- Поля npd, receipt_task_id, receipt_url, receipt_at, receipt_refund_task_id живут в
-- bookings.payment / package_purchases.payment (JSONB).
ALTER TABLE user_preferences ADD COLUMN IF NOT EXISTS npd_receipts BOOLEAN NOT NULL DEFAULT false;
-- ------------------------------ Рост (F7): ИИ-администратор в Telegram ---
-- Те же изменения для существующей базы — миграция 2026_10_17_telegram_admin.sql.
-- Поле страницы: бот отвечает гостям, показывает свободное время и записывает (Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS ai_admin BOOLEAN NOT NULL DEFAULT false;

-- Чат гостя, привязанный к странице командой /start p_<slug>: один чат — одна страница.
-- Данные — по минимуму: имя и ник из Telegram, последние ≤ 10 сообщений (контекст ИИ).
-- /stop и блокировка бота (403) удаляют строку.
CREATE TABLE IF NOT EXISTS telegram_guest_chats (
    chat_id          BIGINT        PRIMARY KEY,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    guest_name       TEXT          NOT NULL DEFAULT '',
    username         TEXT          NOT NULL DEFAULT '',
    history          JSONB         NOT NULL DEFAULT '[]'::jsonb,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS telegram_guest_chats_page_idx ON telegram_guest_chats (page_id);

-- Вопросы гостей, переданные специалисту (escalate): уведомление telegram.question,
-- ответ — telegram.reply.
CREATE TABLE IF NOT EXISTS telegram_questions (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    chat_id          BIGINT        NOT NULL,
    guest_name       TEXT          NOT NULL DEFAULT '',
    username         TEXT          NOT NULL DEFAULT '',
    text             TEXT          NOT NULL,
    status           TEXT          NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'answered')),
    answer           TEXT          NOT NULL DEFAULT '',
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    answered_at      TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS telegram_questions_owner_idx ON telegram_questions (owner_id, id DESC);

-- --------------------------------------------------------------- Триггер ---
CREATE OR REPLACE FUNCTION selfvoy_touch_updated_at() RETURNS trigger AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS user_memory_touch ON user_memory;
CREATE TRIGGER user_memory_touch
    BEFORE UPDATE ON user_memory
    FOR EACH ROW EXECUTE FUNCTION selfvoy_touch_updated_at();

DROP TRIGGER IF EXISTS user_preferences_touch ON user_preferences;
CREATE TRIGGER user_preferences_touch
    BEFORE UPDATE ON user_preferences
    FOR EACH ROW EXECUTE FUNCTION selfvoy_touch_updated_at();

-- При новом сообщении обновляем chats.last_message_at
CREATE OR REPLACE FUNCTION selfvoy_touch_chat_last_message() RETURNS trigger AS $$
BEGIN
    UPDATE chats SET last_message_at = NEW.created_at WHERE id = NEW.chat_id;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS messages_touch_chat ON messages;
CREATE TRIGGER messages_touch_chat
    AFTER INSERT ON messages
    FOR EACH ROW EXECUTE FUNCTION selfvoy_touch_chat_last_message();

-- Новый пользователь сразу получает строку настроек
CREATE OR REPLACE FUNCTION selfvoy_create_defaults() RETURNS trigger AS $$
BEGIN
    INSERT INTO user_preferences (user_id) VALUES (NEW.id) ON CONFLICT DO NOTHING;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS users_create_defaults ON users;
CREATE TRIGGER users_create_defaults
    AFTER INSERT ON users
    FOR EACH ROW EXECUTE FUNCTION selfvoy_create_defaults();

-- Функции с прежним префиксом (до переименования продукта в Selfvoy): триггеры
-- выше уже пересозданы на selfvoy_*, старые копии на существующей базе не нужны.
DROP FUNCTION IF EXISTS aura_touch_updated_at();
DROP FUNCTION IF EXISTS aura_touch_chat_last_message();
DROP FUNCTION IF EXISTS aura_create_defaults();

COMMIT;

-- ----------------------------------------------------------------------------
--  Полезные представления (используются ChatManager'ом)
-- ----------------------------------------------------------------------------
-- DROP + CREATE, а не CREATE OR REPLACE: у представления меняется набор колонок.
DROP VIEW IF EXISTS chat_previews;
CREATE VIEW chat_previews AS
SELECT
    c.id            AS chat_id,
    c.kind,
    c.title,
    c.created_by,
    c.created_at,
    c.last_message_at,
    m.body          AS last_message_body,
    m.kind          AS last_message_kind,
    m.sender_id     AS last_message_sender_id,
    u.display_name  AS last_message_sender
FROM chats c
LEFT JOIN LATERAL (
    SELECT id, body, kind, sender_id, created_at
    FROM messages
    WHERE chat_id = c.id
    ORDER BY created_at DESC, id DESC
    LIMIT 1
) m ON TRUE
LEFT JOIN users u ON u.id = m.sender_id;

CREATE OR REPLACE VIEW active_sessions AS
SELECT s.id, s.user_id, s.device, s.created_at, s.expires_at
FROM sessions s
WHERE s.revoked_at IS NULL AND s.expires_at > now();

-- -------------------------------------------------------- OAuth Auth ----
-- Привязка аккаунтов OAuth (Google, GitHub, Yandex, VK, Discord) и одноразовые состояния.
CREATE TABLE IF NOT EXISTS user_oauth_accounts (
    id               BIGSERIAL    PRIMARY KEY,
    user_id          BIGINT       NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider         TEXT         NOT NULL,
    provider_user_id TEXT         NOT NULL,
    email            TEXT         NOT NULL DEFAULT '',
    created_at       TIMESTAMPTZ  NOT NULL DEFAULT now(),
    CONSTRAINT user_oauth_accounts_uniq UNIQUE (provider, provider_user_id)
);

CREATE INDEX IF NOT EXISTS user_oauth_accounts_user_idx ON user_oauth_accounts (user_id);
CREATE INDEX IF NOT EXISTS user_oauth_accounts_email_idx ON user_oauth_accounts (email) WHERE email <> '';

CREATE TABLE IF NOT EXISTS auth_oauth_states (
    state        TEXT          PRIMARY KEY,
    provider     TEXT          NOT NULL,
    verifier     TEXT          NOT NULL,
    redirect_uri TEXT          NOT NULL DEFAULT '',
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ   NOT NULL,
    used_at      TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS auth_oauth_states_expires_idx ON auth_oauth_states (expires_at);

