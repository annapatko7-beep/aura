-- ============================================================================
--  Aura — миграция 2FA (этап 6)
--  Применение к существующей БД:
--    psql "$AURA_DATABASE_URL" -f schema/migrations/2026_09_20_two_factor.sql
--  Для свежей БД достаточно schema/schema.sql — миграция идемпотентна.
-- ============================================================================

BEGIN;

-- Настройка TOTP 2FA: секрет хранится зашифрованным (AES-256-GCM, ключ — env
-- AURA_2FA_KEY); enabled=true только после подтверждения одноразовым кодом.
CREATE TABLE IF NOT EXISTS two_factor_settings (
    user_id           BIGINT        PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    secret_encrypted  TEXT          NOT NULL,
    enabled           BOOLEAN       NOT NULL DEFAULT FALSE,
    last_used_counter BIGINT        NOT NULL DEFAULT 0,
    created_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    enabled_at        TIMESTAMPTZ,
    updated_at        TIMESTAMPTZ   NOT NULL DEFAULT now()
);

-- Резервные коды восстановления: только SHA-256(code), код одноразовый.
CREATE TABLE IF NOT EXISTS recovery_codes (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    code_hash      TEXT          NOT NULL,
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    used_at        TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS recovery_codes_user_idx ON recovery_codes (user_id, code_hash);

-- Доверенные устройства: освобождают устройство от повторного ввода кода на
-- 90 дней (или до отзыва).
CREATE TABLE IF NOT EXISTS trusted_devices (
    id             UUID          PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id        BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    device_hash    TEXT          NOT NULL,
    device         TEXT          NOT NULL DEFAULT '',
    remote_addr    TEXT          NOT NULL DEFAULT '',
    created_at     TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at     TIMESTAMPTZ   NOT NULL,
    last_used_at   TIMESTAMPTZ,
    revoked_at     TIMESTAMPTZ,
    UNIQUE (user_id, device_hash)
);

CREATE INDEX IF NOT EXISTS trusted_devices_user_idx ON trusted_devices (user_id);

COMMIT;
