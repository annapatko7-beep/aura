-- Аккаунты PostgreSQL, обошедшие подтверждение email.
--
-- До исправления в database_pg.cpp (createUser без email_verified) новый
-- аккаунт получал email_verified = TRUE из DEFAULT схемы: auth.verifyEmail
-- уходил в ветку «уже подтверждён», а вход работал без кода. Встроенное
-- хранилище (JSON) ошибки не имело.
--
-- Признак затронутого аккаунта: в audit_logs есть «register» (регистрация
-- через auth.register), но нет «email_verified», а в users стоит TRUE.
-- Старые аккаунты (созданные до подтверждения email, seed, ручные вставки)
-- события «register» не имеют и сюда не попадают.
--
-- Отчёт (ничего не меняет):
--   psql "$DATABASE_URL" -f tools/sql/unverified_accounts.sql
-- Сбросить подтверждение затронутым аккаунтам (в транзакции):
--   psql "$DATABASE_URL" -v apply=1 -f tools/sql/unverified_accounts.sql
-- Плюс отозвать их сессии и refresh-токены — следующий вход попросит код
-- (auth.login → email_not_verified, клиент покажет экран кода):
--   psql "$DATABASE_URL" -v apply=1 -v revoke=1 -f tools/sql/unverified_accounts.sql
--
-- Флаги — булевы: 1/true/on/yes включают, 0/false/off/no (или отсутствие)
-- выключают; другое значение — ошибка. -v apply=0 — тот же отчёт.
-- revoke без apply ничего не делает.
--
-- ВАЖНО: после сброса аккаунт войдёт только по коду из письма. Сервер должен
-- реально отправлять почту (SELFVOY_MAIL_DRIVER=smtp): с драйвером log код никуда
-- не уходит и не пишется в лог — пользователи не смогут подтвердить email,
-- а с revoke=1 ещё и потеряют текущие сессии.
\set ON_ERROR_STOP on

-- Флаги: psql-переменная задана -> разбираем значение; нет -> выключено.
\if :{?apply}
\else
\set apply 0
\endif
\if :{?revoke}
\else
\set revoke 0
\endif
SELECT lower(trim(:'apply')) IN ('1', 'true', 'on', 'yes', 't', 'y') AS do_apply,
       lower(trim(:'apply')) IN ('1', 'true', 'on', 'yes', 't', 'y', '0', 'false', 'off', 'no', 'f', 'n', '') AS apply_ok,
       lower(trim(:'revoke')) IN ('1', 'true', 'on', 'yes', 't', 'y') AS do_revoke,
       lower(trim(:'revoke')) IN ('1', 'true', 'on', 'yes', 't', 'y', '0', 'false', 'off', 'no', 'f', 'n', '') AS revoke_ok
\gset
\if :apply_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'apply: ожидается 1/0 (true/false, on/off, yes/no)'; END $$;
\endif
\if :revoke_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'revoke: ожидается 1/0 (true/false, on/off, yes/no)'; END $$;
\endif

-- Признак затронутого аккаунта (то же условие повторяется под блокировкой ниже).
CREATE TEMP TABLE affected_accounts AS
SELECT u.id, u.email, u.display_name, u.created_at, u.last_seen_at
FROM users u
WHERE u.email_verified
  AND EXISTS (SELECT 1 FROM audit_logs a WHERE a.user_id = u.id AND a.kind = 'register')
  AND NOT EXISTS (SELECT 1 FROM audit_logs a WHERE a.user_id = u.id AND a.kind = 'email_verified');

\echo 'Аккаунты без подтверждённого email (email_verified = true без кода):'
SELECT id, email, display_name, created_at, last_seen_at FROM affected_accounts ORDER BY id;
SELECT count(*) AS affected FROM affected_accounts;

\if :do_apply
\echo 'Внимание: после сброса вход только по коду из письма — у сервера должен быть SELFVOY_MAIL_DRIVER=smtp (с log код не дойдёт).'
BEGIN;
-- Перепроверка под блокировкой строк: между отчётом и сбросом аккаунт мог
-- подтвердить email или поменяться — такие строки не трогаем.
SELECT count(*) AS locked
FROM (SELECT u.id FROM users u WHERE u.id IN (SELECT id FROM affected_accounts) ORDER BY u.id FOR UPDATE) l;
CREATE TEMP TABLE reset_accounts (id BIGINT PRIMARY KEY) ON COMMIT DROP;
WITH reset AS (
    UPDATE users u SET email_verified = false
    WHERE u.id IN (SELECT id FROM affected_accounts)
      AND u.email_verified
      AND EXISTS (SELECT 1 FROM audit_logs a WHERE a.user_id = u.id AND a.kind = 'register')
      AND NOT EXISTS (SELECT 1 FROM audit_logs a WHERE a.user_id = u.id AND a.kind = 'email_verified')
    RETURNING u.id
)
INSERT INTO reset_accounts (id) SELECT id FROM reset;
SELECT count(*) AS reset FROM reset_accounts;
INSERT INTO audit_logs (user_id, kind, detail)
SELECT id, 'email_unverified_reset', '{"reason":"pg_default_bug"}'::jsonb FROM reset_accounts;
\if :do_revoke
UPDATE sessions SET revoked_at = now() WHERE user_id IN (SELECT id FROM reset_accounts) AND revoked_at IS NULL;
UPDATE refresh_tokens SET revoked_at = now() WHERE user_id IN (SELECT id FROM reset_accounts) AND revoked_at IS NULL;
\echo 'Сессии и refresh-токены сброшенных аккаунтов отозваны.'
\endif
COMMIT;
\echo 'Готово: сброшенным аккаунтам нужно подтвердить email при следующем входе (число — в «reset» выше).'
\else
\if :do_revoke
\echo 'revoke без apply ничего не делает.'
\endif
\echo 'Только отчёт, ничего не изменено. Сбросить подтверждение: -v apply=1 (и -v revoke=1, чтобы разлогинить).'
\echo 'Перед сбросом проверьте почту сервера: нужен SELFVOY_MAIL_DRIVER=smtp — с log код подтверждения не дойдёт.'
\endif
