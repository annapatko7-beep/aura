-- ============================================================================
--  Aura — миграция уведомлений (этап 13)
--  In-app «входящая» (notifications) + устройства push (push_devices).
--  Применение к существующей БД:
--    psql "$AURA_DATABASE_URL" -f schema/migrations/2026_09_20_notifications.sql
--  Для свежей БД достаточно schema/schema.sql — миграция идемпотентна.
-- ============================================================================

BEGIN;

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

CREATE TABLE IF NOT EXISTS push_devices (
    id           BIGSERIAL     PRIMARY KEY,
    user_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    platform     TEXT          NOT NULL DEFAULT 'apns'
                 CHECK (platform IN ('apns', 'webhook', 'dev')),
    token        TEXT          NOT NULL,
    enabled      BOOLEAN       NOT NULL DEFAULT TRUE,
    created_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    last_used_at TIMESTAMPTZ,
    UNIQUE (user_id, token)
);

CREATE INDEX IF NOT EXISTS push_devices_user_idx ON push_devices (user_id);

COMMIT;
