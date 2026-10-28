-- 2026_10_20_oauth_auth.sql
-- Регистрация и авторизация через внешних OAuth-провайдеров (Google, GitHub, Yandex, VK ID, Discord).

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
