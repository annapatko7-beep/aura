-- Рост (S1a): тариф Pro, Telegram-бот, служебное состояние, шифр токена ссылки
-- управления. Идемпотентно. См. docs/DATABASE.md.

ALTER TABLE users ADD COLUMN IF NOT EXISTS plan_until TIMESTAMPTZ;
ALTER TABLE users ADD COLUMN IF NOT EXISTS telegram_chat_id BIGINT;
ALTER TABLE users ADD COLUMN IF NOT EXISTS telegram_username TEXT NOT NULL DEFAULT '';
CREATE UNIQUE INDEX IF NOT EXISTS users_telegram_chat_key ON users (telegram_chat_id)
    WHERE telegram_chat_id IS NOT NULL;

ALTER TABLE bookings ADD COLUMN IF NOT EXISTS manage_token_enc TEXT;

CREATE TABLE IF NOT EXISTS booking_telegram (
    booking_id       BIGINT        NOT NULL REFERENCES bookings(id) ON DELETE CASCADE,
    chat_id          BIGINT        NOT NULL,
    linked_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    PRIMARY KEY (booking_id, chat_id)
);
CREATE INDEX IF NOT EXISTS booking_telegram_chat_idx ON booking_telegram (chat_id);

CREATE TABLE IF NOT EXISTS server_state (
    key              TEXT          PRIMARY KEY,
    value            TEXT          NOT NULL,
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
