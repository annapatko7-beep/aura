-- Исправления по итогам проверки (защита и скорость). Идемпотентно.
-- См. docs/DATABASE.md.

-- Новые аккаунты не должны считаться подтверждёнными по умолчанию (fail closed):
-- сервер и так пишет email_verified явно, но любая будущая вставка без колонки
-- больше не пропустит подтверждение email.
ALTER TABLE users ALTER COLUMN email_verified SET DEFAULT FALSE;

-- Конец последней активной встречи записи: занятость владельца, лимит в день и
-- счётчик «предстоит» читают только записи с будущими встречами (прошедшие
-- записи остаются active навсегда и раньше вытесняли будущие из выборки).
ALTER TABLE bookings ADD COLUMN IF NOT EXISTS ends_at TIMESTAMPTZ;
UPDATE bookings b
SET ends_at = (
    SELECT max((o ->> 'end')::timestamptz)
    FROM jsonb_array_elements(b.occurrences) o
    WHERE o ->> 'status' = 'active'
)
WHERE b.ends_at IS NULL;
CREATE INDEX IF NOT EXISTS bookings_owner_upcoming_idx ON bookings (owner_id, ends_at) WHERE status = 'active';
CREATE INDEX IF NOT EXISTS bookings_page_upcoming_idx ON bookings (page_id, ends_at) WHERE status = 'active';

-- Счётчики попыток: без WAL (каждая попытка входа не ждёт fsync) и с индексом
-- для порционной чистки истёкших окон.
ALTER TABLE rate_limits SET UNLOGGED;
CREATE INDEX IF NOT EXISTS rate_limits_window_idx ON rate_limits (window_start);
