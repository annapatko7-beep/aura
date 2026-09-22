-- Онбординг-опрос после регистрации: день рождения, аллергии, флаг прохождения.
-- Для свежих БД эти колонки уже есть в schema.sql.
ALTER TABLE user_preferences ADD COLUMN IF NOT EXISTS birthday  TEXT    NOT NULL DEFAULT '';
ALTER TABLE user_preferences ADD COLUMN IF NOT EXISTS allergies TEXT[]  NOT NULL DEFAULT '{}';
ALTER TABLE user_preferences ADD COLUMN IF NOT EXISTS onboarded BOOLEAN NOT NULL DEFAULT FALSE;
