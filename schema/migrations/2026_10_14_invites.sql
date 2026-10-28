-- Приглашения по ссылке: владелец договаривается о встрече с человеком без Selfvoy. См. docs/PROTOCOL.md (invite.*).
-- В БД только sha256 токена (сама ссылка показывается владельцу один раз).
-- slots — предложенные интервалы [{start,end}] (ISO UTC); chosen — выбранный гостем.
-- Статус expired не хранится: это open с истёкшим expires_at.
CREATE TABLE IF NOT EXISTS meeting_invites (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash       TEXT          NOT NULL UNIQUE,
    topic            TEXT          NOT NULL,
    note             TEXT          NOT NULL DEFAULT '',
    duration_minutes INTEGER       NOT NULL CHECK (duration_minutes BETWEEN 15 AND 240),
    slots            JSONB         NOT NULL DEFAULT '[]'::jsonb,
    status           TEXT          NOT NULL DEFAULT 'open'
                     CHECK (status IN ('open', 'accepted', 'declined', 'cancelled')),
    guest_name       TEXT          NOT NULL DEFAULT '',
    guest_email      TEXT          NOT NULL DEFAULT '',
    guest_comment    TEXT          NOT NULL DEFAULT '',
    chosen           JSONB         NOT NULL DEFAULT '{}'::jsonb,
    expires_at       TIMESTAMPTZ   NOT NULL,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    responded_at     TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS meeting_invites_owner_idx ON meeting_invites (owner_id, id DESC);
