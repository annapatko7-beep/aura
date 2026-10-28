-- Рост (S2a): цена, предоплата и политика отмены страницы записи, оплата записи,
-- магазин ЮKassa владельца (F4). Идемпотентно. См. docs/DATABASE.md.

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
