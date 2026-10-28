-- Роли пользователей для админ-панели: user (по умолчанию), moderator, admin.
-- Первые администраторы назначаются через SELFVOY_ADMIN_EMAILS (см. docs/SETUP.md).
ALTER TABLE users ADD COLUMN IF NOT EXISTS role TEXT NOT NULL DEFAULT 'user';
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'users_role_check') THEN
        ALTER TABLE users ADD CONSTRAINT users_role_check CHECK (role IN ('user', 'moderator', 'admin'));
    END IF;
END $$;
CREATE INDEX IF NOT EXISTS users_role_idx ON users (role) WHERE role <> 'user';
