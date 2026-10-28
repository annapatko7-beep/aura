-- F6: ICS-подписка владельца на свои записи и принятые приглашения.
-- calendar.feedReset выдаёт ссылку <SELFVOY_SERVER_PUBLIC_URL>/ics/<токен>.ics;
-- в БД — только hex sha256 токена (NULL — подписка выключена). Идемпотентно.
-- См. docs/DATABASE.md, docs/PROTOCOL.md («Календарь: ICS-подписка»).

ALTER TABLE users ADD COLUMN IF NOT EXISTS ics_token_hash TEXT;
-- Уникальный индекс, а не ограничение в ADD COLUMN: повторный прогон не создаёт дубль.
CREATE UNIQUE INDEX IF NOT EXISTS users_ics_token_hash_key ON users (ics_token_hash);
