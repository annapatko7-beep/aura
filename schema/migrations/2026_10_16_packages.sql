-- Рост (S2b): пакеты/абонементы и второй получатель извещений гостя (F5).
-- Идемпотентно. См. docs/DATABASE.md.

-- Предложения пакетов страницы записи: [{id, title, sessions, price, valid_days}], не больше 5.
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS packages JSONB NOT NULL DEFAULT '[]'::jsonb;

-- Покупки пакетов: снимок предложения (название, занятия, цена, срок) на момент покупки
-- или выдачи владельцем. В БД — sha256 токена ссылки #/p/<токен> и сам токен,
-- зашифрованный AES-256-GCM (AAD "selfvoy-package-token"), — для писем гостю.
CREATE TABLE IF NOT EXISTS package_purchases (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    offer_id         INTEGER       NOT NULL DEFAULT 0,
    title            TEXT          NOT NULL,
    sessions_total   INTEGER       NOT NULL CHECK (sessions_total BETWEEN 1 AND 1000),
    sessions_used    INTEGER       NOT NULL DEFAULT 0 CHECK (sessions_used >= 0),
    valid_days       INTEGER       NOT NULL DEFAULT 90 CHECK (valid_days BETWEEN 1 AND 3650),
    price            BIGINT        NOT NULL DEFAULT 0 CHECK (price BETWEEN 0 AND 100000000),
    currency         TEXT          NOT NULL DEFAULT 'RUB' CHECK (currency IN ('RUB', 'USD', 'EUR', 'KZT', 'BYN')),
    valid_until      TIMESTAMPTZ,  -- NULL — ещё не активирован (ждёт оплату)
    status           TEXT          NOT NULL DEFAULT 'pending'
                     CHECK (status IN ('pending', 'active', 'used', 'expired', 'cancelled')),
    payment          JSONB,        -- оплата (как bookings.payment, kind = package); NULL — без оплаты
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL DEFAULT '',
    cc_email         TEXT          NOT NULL DEFAULT '',
    token_hash       TEXT          NOT NULL UNIQUE,
    token_enc        TEXT,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS package_purchases_owner_idx ON package_purchases (owner_id, id DESC);
-- Планировщик: ожидающие оплату (по сроку) и истекающие активные пакеты.
CREATE INDEX IF NOT EXISTS package_purchases_pending_idx ON package_purchases ((payment->>'due_at'))
    WHERE status = 'pending';
CREATE INDEX IF NOT EXISTS package_purchases_valid_idx ON package_purchases (valid_until) WHERE status = 'active';

-- Записи: второй получатель извещений гостя (копия, например родителю) и пакет записи.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS cc_email TEXT NOT NULL DEFAULT '';
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS package_id BIGINT REFERENCES package_purchases(id) ON DELETE SET NULL;
CREATE INDEX IF NOT EXISTS bookings_package_idx ON bookings (package_id) WHERE package_id IS NOT NULL;
