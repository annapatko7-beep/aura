-- Рост (S1b): напоминания гостю (F1), подпись Selfvoy и реферальная награда (F3).
-- Идемпотентно. См. docs/DATABASE.md.

-- Поля страницы записи: напоминания гостю (бесплатно) и скрытие подписи (Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS guest_reminders BOOLEAN NOT NULL DEFAULT true;
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS hide_branding BOOLEAN NOT NULL DEFAULT false;

-- Напоминания гостю за сутки (24h) и за 2 часа (2h) до каждой активной встречи.
-- Создаются при записи и переносе, неотправленные удаляются при отмене/переносе
-- встречи; sent_at ставит атомарный захват планировщика (FOR UPDATE SKIP LOCKED).
CREATE TABLE IF NOT EXISTS booking_reminders (
    id               BIGSERIAL     PRIMARY KEY,
    booking_id       BIGINT        REFERENCES bookings(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL,
    occurrence_start TIMESTAMPTZ   NOT NULL,
    kind             TEXT          NOT NULL CHECK (kind IN ('24h', '2h')),
    due_at           TIMESTAMPTZ   NOT NULL,
    sent_at          TIMESTAMPTZ,
    UNIQUE (booking_id, occurrence_start, kind)
);
CREATE INDEX IF NOT EXISTS booking_reminders_due_idx ON booking_reminders (due_at) WHERE sent_at IS NULL;

-- Награда за приглашённого (+30 дней Pro обоим) начисляется один раз.
ALTER TABLE referrals ADD COLUMN IF NOT EXISTS rewarded_at TIMESTAMPTZ;
