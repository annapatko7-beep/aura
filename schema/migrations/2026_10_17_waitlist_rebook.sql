-- Рост, этап 2: лист ожидания и «пора на повторный приём» (F8), чеки «Мой налог» (F10).
-- Идемпотентно. См. docs/DATABASE.md.

-- Поля страницы записи: лист ожидания (Pro), сколько минут освободившееся время
-- держится за человеком из листа, через сколько дней после последней встречи
-- напомнить гостю записаться снова (0 — выкл., иначе 7–180; Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS waitlist BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS waitlist_hold_minutes INTEGER NOT NULL DEFAULT 60
    CHECK (waitlist_hold_minutes BETWEEN 15 AND 240);
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS rebook_after_days INTEGER NOT NULL DEFAULT 0
    CHECK (rebook_after_days = 0 OR rebook_after_days BETWEEN 7 AND 180);

-- «Пора записаться снова»: когда планировщик обработал запись (NULL — ещё нет) и итог.
-- Ровно один раз на запись: захват — UPDATE … FOR UPDATE SKIP LOCKED.
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS rebook_sent_at TIMESTAMPTZ;
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS rebook_status TEXT NOT NULL DEFAULT ''
    CHECK (rebook_status IN ('', 'claimed', 'sent', 'skipped', 'cancelled'));
CREATE INDEX IF NOT EXISTS bookings_rebook_idx ON bookings (ends_at)
    WHERE status = 'active' AND rebook_sent_at IS NULL;

-- Лист ожидания страницы записи. Освободившееся время держится за самой старой
-- подходящей записью (offer_*, до offer_expires_at); гость забирает его по ссылке
-- #/w/<токен>: в БД — sha256 токена и сам токен, зашифрованный AES-256-GCM
-- (AAD "selfvoy-waitlist-token"), — для писем и бота.
CREATE TABLE IF NOT EXISTS waitlist_entries (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    guest_name       TEXT          NOT NULL,
    guest_email      TEXT          NOT NULL,
    days             JSONB         NOT NULL DEFAULT '[]'::jsonb,  -- местные даты владельца; [] — любой день
    part_of_day      TEXT          NOT NULL DEFAULT 'any'
                     CHECK (part_of_day IN ('any', 'morning', 'afternoon', 'evening')),
    comment          TEXT          NOT NULL DEFAULT '',
    status           TEXT          NOT NULL DEFAULT 'waiting'
                     CHECK (status IN ('waiting', 'offered', 'booked', 'left', 'expired')),
    token_hash       TEXT          NOT NULL UNIQUE,
    token_enc        TEXT,
    telegram_chat_id BIGINT,       -- подписка на предложения в боте; NULL — нет
    offer_start      TIMESTAMPTZ,  -- предложенное (держащееся) время; NULL — предложения не было
    offer_end        TIMESTAMPTZ,
    offer_day        TEXT          NOT NULL DEFAULT '',
    offer_local      TEXT          NOT NULL DEFAULT '',
    offer_expires_at TIMESTAMPTZ,
    offer_status     TEXT          NOT NULL DEFAULT ''
                     CHECK (offer_status IN ('', 'offered', 'claimed', 'expired', 'cancelled')),
    booking_id       BIGINT        REFERENCES bookings(id) ON DELETE SET NULL,
    queued_at        TIMESTAMPTZ   NOT NULL DEFAULT now(),
    expires_at       TIMESTAMPTZ   NOT NULL,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
-- Один email — одна активная запись листа на страницу.
CREATE UNIQUE INDEX IF NOT EXISTS waitlist_entries_active_email_key ON waitlist_entries (page_id, lower(guest_email))
    WHERE status IN ('waiting', 'offered');
CREATE INDEX IF NOT EXISTS waitlist_entries_owner_idx ON waitlist_entries (owner_id, id DESC);
CREATE INDEX IF NOT EXISTS waitlist_entries_queue_idx ON waitlist_entries (page_id, queued_at) WHERE status = 'waiting';
-- Держания (занятость владельца) и планировщик: истекающие предложения и записи.
CREATE INDEX IF NOT EXISTS waitlist_entries_holds_idx ON waitlist_entries (owner_id, offer_expires_at)
    WHERE status = 'offered';
CREATE INDEX IF NOT EXISTS waitlist_entries_offer_due_idx ON waitlist_entries (offer_expires_at)
    WHERE status = 'offered';
CREATE INDEX IF NOT EXISTS waitlist_entries_expiry_idx ON waitlist_entries (expires_at) WHERE status = 'waiting';
CREATE INDEX IF NOT EXISTS waitlist_entries_telegram_idx ON waitlist_entries (telegram_chat_id)
    WHERE telegram_chat_id IS NOT NULL;

-- F10 («Мой налог»): настройка владельца «я самозанятый, напоминать выдать чек» (Pro).
-- Поля npd, receipt_task_id, receipt_url, receipt_at, receipt_refund_task_id живут в
-- bookings.payment / package_purchases.payment (JSONB).
ALTER TABLE user_preferences ADD COLUMN IF NOT EXISTS npd_receipts BOOLEAN NOT NULL DEFAULT false;
