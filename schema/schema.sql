-- ============================================================================
--  Aura — схема PostgreSQL
--  Таблицы ровно по скелету проекта:
--    Users, Chats, ChatMembers, Messages, UserMemory, UserPreferences, Sessions
--
--  Применение:  psql "$AURA_DATABASE_URL" -f schema/schema.sql
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
    email_verified BOOLEAN       NOT NULL DEFAULT TRUE,  -- подтверждён ли email
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_seen_at   TIMESTAMPTZ
);

-- Email уникален без учёта регистра (citext не требуется)
CREATE UNIQUE INDEX IF NOT EXISTS users_email_lower_key ON users (lower(email));

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
-- AURA_2FA_KEY); enabled=true только после подтверждения одноразовым кодом.
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
    payload        JSONB         NOT NULL DEFAULT '{}'::jsonb,  -- действия Ауры, A2A-предложения
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
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ------------------------------------------------------ ToolPermissions ----
-- Разрешения пользователя на инструменты Ауры (этап 8, ядро безопасности).
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
-- Действия, требующие явного подтверждения (барьер подтверждения). Аура
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
-- Токены хранятся ТОЛЬКО зашифрованными (AES-256-GCM, ключ — env AURA_2FA_KEY),
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

-- --------------------------------------------------------------- Триггер ---
CREATE OR REPLACE FUNCTION aura_touch_updated_at() RETURNS trigger AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS user_memory_touch ON user_memory;
CREATE TRIGGER user_memory_touch
    BEFORE UPDATE ON user_memory
    FOR EACH ROW EXECUTE FUNCTION aura_touch_updated_at();

DROP TRIGGER IF EXISTS user_preferences_touch ON user_preferences;
CREATE TRIGGER user_preferences_touch
    BEFORE UPDATE ON user_preferences
    FOR EACH ROW EXECUTE FUNCTION aura_touch_updated_at();

-- При новом сообщении обновляем chats.last_message_at
CREATE OR REPLACE FUNCTION aura_touch_chat_last_message() RETURNS trigger AS $$
BEGIN
    UPDATE chats SET last_message_at = NEW.created_at WHERE id = NEW.chat_id;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS messages_touch_chat ON messages;
CREATE TRIGGER messages_touch_chat
    AFTER INSERT ON messages
    FOR EACH ROW EXECUTE FUNCTION aura_touch_chat_last_message();

-- Новый пользователь сразу получает строку настроек
CREATE OR REPLACE FUNCTION aura_create_defaults() RETURNS trigger AS $$
BEGIN
    INSERT INTO user_preferences (user_id) VALUES (NEW.id) ON CONFLICT DO NOTHING;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS users_create_defaults ON users;
CREATE TRIGGER users_create_defaults
    AFTER INSERT ON users
    FOR EACH ROW EXECUTE FUNCTION aura_create_defaults();

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
