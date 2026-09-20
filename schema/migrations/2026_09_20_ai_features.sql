-- ============================================================================
--  Aura — миграция AI-возможностей (этапы 8–9)
--  Этап 8: разрешения на инструменты, барьер подтверждения, задачи.
--  Этап 9: интеграции (Google OAuth, токены зашифрованы) + OAuth-состояния.
--  Применение к существующей БД:
--    psql "$AURA_DATABASE_URL" -f schema/migrations/2026_09_20_ai_features.sql
--  Для свежей БД достаточно schema/schema.sql — миграция идемпотентна.
-- ============================================================================

BEGIN;

-- Разрешения пользователя на инструменты Ауры. mode: allow | ask | deny.
-- Если строки нет, режим выводится из опасности инструмента (опасные → ask).
CREATE TABLE IF NOT EXISTS tool_permissions (
    user_id     BIGINT      NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    tool        TEXT        NOT NULL,
    mode        TEXT        NOT NULL DEFAULT 'ask'
                CHECK (mode IN ('allow', 'ask', 'deny')),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, tool)
);

-- Отложенные действия (барьер подтверждения): Аура создаёт запись вместо
-- исполнения; выполнение — только после confirmation.approve.
CREATE TABLE IF NOT EXISTS pending_actions (
    id           BIGSERIAL     PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    chat_id      BIGINT        REFERENCES chats(id) ON DELETE SET NULL,
    tool         TEXT          NOT NULL,
    args         JSONB         NOT NULL DEFAULT '{}'::jsonb,
    summary      TEXT          NOT NULL DEFAULT '',
    status       TEXT          NOT NULL DEFAULT 'pending'
                 CHECK (status IN ('pending', 'approved', 'denied', 'executed', 'failed', 'expired')),
    result       JSONB         NOT NULL DEFAULT '{}'::jsonb,
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    resolved_at  TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS pending_actions_user_idx ON pending_actions (user_id, status, id DESC);

-- Задачи и напоминания: remind_at — когда планировщик шлёт уведомление.
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

-- Подключённые внешние сервисы (этап 9): токены только зашифрованными.
CREATE TABLE IF NOT EXISTS integration_connections (
    id              BIGSERIAL     PRIMARY KEY,
    user_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider        TEXT          NOT NULL,
    account         TEXT          NOT NULL DEFAULT '',
    scope           TEXT          NOT NULL DEFAULT '',
    token_encrypted TEXT          NOT NULL,
    status          TEXT          NOT NULL DEFAULT 'active'
                    CHECK (status IN ('active', 'expired', 'revoked', 'error')),
    last_error      TEXT          NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_used_at    TIMESTAMPTZ,
    UNIQUE (user_id, provider)
);

CREATE INDEX IF NOT EXISTS integration_connections_user_idx ON integration_connections (user_id);

-- Одноразовые состояния OAuth 2.0 + PKCE.
CREATE TABLE IF NOT EXISTS integration_oauth_states (
    state        TEXT          PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider     TEXT          NOT NULL,
    verifier     TEXT          NOT NULL,
    redirect_uri TEXT          NOT NULL DEFAULT '',
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ   NOT NULL,
    used_at      TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS integration_oauth_states_user_idx ON integration_oauth_states (user_id);

COMMIT;
