-- Страницы записи (booking.*) и рефералы (referral.*). См. docs/PROTOCOL.md и docs/DATABASE.md.
-- Страницы записи (booking.*): постоянная публичная ссылка владельца (#/b/<slug>),
-- гости без Selfvoy сами записываются на свободное время. См. docs/PROTOCOL.md.
CREATE TABLE IF NOT EXISTS booking_pages (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    slug             TEXT          NOT NULL UNIQUE,
    title            TEXT          NOT NULL,
    description      TEXT          NOT NULL DEFAULT '',
    duration_minutes INTEGER       NOT NULL CHECK (duration_minutes BETWEEN 15 AND 240),
    days_ahead       INTEGER       NOT NULL DEFAULT 14 CHECK (days_ahead BETWEEN 1 AND 60),
    max_per_day      INTEGER       NOT NULL DEFAULT 4 CHECK (max_per_day BETWEEN 1 AND 16),
    notice_hours     INTEGER       NOT NULL DEFAULT 2 CHECK (notice_hours BETWEEN 0 AND 168),
    max_series       INTEGER       NOT NULL DEFAULT 1 CHECK (max_series BETWEEN 1 AND 12),
    weekends         BOOLEAN       NOT NULL DEFAULT false,
    status           TEXT          NOT NULL DEFAULT 'active'
                     CHECK (status IN ('active', 'paused', 'archived')),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS booking_pages_owner_idx ON booking_pages (owner_id, id DESC);

-- Записи гостей. occurrences — встречи (одна или еженедельная серия):
-- [{start, end, day, status: active|cancelled, task_id}]. В БД только sha256
-- токена ссылки управления (#/m/<токен>).
CREATE TABLE IF NOT EXISTS bookings (
    id               BIGSERIAL     PRIMARY KEY,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    manage_hash      TEXT          NOT NULL UNIQUE,
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL DEFAULT '',
    guest_comment    TEXT          NOT NULL DEFAULT '',
    occurrences      JSONB         NOT NULL DEFAULT '[]'::jsonb,
    status           TEXT          NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'cancelled')),
    cancelled_by     TEXT          NOT NULL DEFAULT '',
    reschedules      INTEGER       NOT NULL DEFAULT 0,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS bookings_owner_idx ON bookings (owner_id, id DESC);
CREATE INDEX IF NOT EXISTS bookings_page_idx ON bookings (page_id, status);

-- Рефералы (referral.*): у каждого пользователя один код; приглашённый
-- записывается при регистрации (auth.register → ref), verified_at — после
-- подтверждения email.
CREATE TABLE IF NOT EXISTS referral_codes (
    user_id          BIGINT        PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    code             TEXT          NOT NULL UNIQUE CHECK (code ~ '^[A-Z0-9]{8}$'),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS referrals (
    id               BIGSERIAL     PRIMARY KEY,
    referrer_id      BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    referred_id      BIGINT        NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
    source           TEXT          NOT NULL DEFAULT 'link' CHECK (source IN ('link', 'booking', 'invite')),
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    verified_at      TIMESTAMPTZ,
    CHECK (referrer_id <> referred_id)
);

CREATE INDEX IF NOT EXISTS referrals_referrer_idx ON referrals (referrer_id, id DESC);
