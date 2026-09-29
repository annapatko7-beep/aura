-- ============================================================================
--  Aura — миграция auth v3 (этап 5)
--  Применение к существующей БД:
--    psql "$AURA_DATABASE_URL" -f schema/migrations/2026_09_19_auth_v3.sql
--  Для свежей БД достаточно schema/schema.sql — миграция идемпотентна.
-- ============================================================================

BEGIN;

-- Существующие пользователи считаются подтверждёнными (аккаунты созданы
-- до введения обязательной верификации email).
ALTER TABLE users ADD COLUMN IF NOT EXISTS email_verified BOOLEAN NOT NULL DEFAULT TRUE;

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

CREATE TABLE IF NOT EXISTS refresh_tokens (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    family_id      UUID          NOT NULL,
    token_hash     TEXT          NOT NULL UNIQUE,
    device         TEXT          NOT NULL DEFAULT '',
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    revoked_at     TIMESTAMPTZ,
    replaced_by    UUID
);

CREATE INDEX IF NOT EXISTS refresh_tokens_family_idx ON refresh_tokens (family_id);
CREATE INDEX IF NOT EXISTS refresh_tokens_user_idx   ON refresh_tokens (user_id);

CREATE TABLE IF NOT EXISTS audit_logs (
    id             BIGSERIAL     PRIMARY KEY,
    user_id        BIGINT        REFERENCES users(id) ON DELETE SET NULL,
    kind           TEXT          NOT NULL,
    detail         JSONB         NOT NULL DEFAULT '{}'::jsonb,
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS audit_logs_user_idx ON audit_logs (user_id, id DESC);

COMMIT;
