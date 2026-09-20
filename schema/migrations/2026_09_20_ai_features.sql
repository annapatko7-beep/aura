-- ============================================================================
--  Aura — миграция AI-возможностей (этап 8)
--  Ядро безопасности: разрешения на инструменты + барьер подтверждения.
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

COMMIT;
