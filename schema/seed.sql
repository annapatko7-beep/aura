-- Демонстрационные данные: два пользователя, чат между ними, память и настройки.
-- Войти под этими аккаунтами нельзя: пароля нет (хэш '!' не совпадает ни с одним
-- паролем) — в репозитории не должно быть известных паролей. Аккаунты для входа
-- на dev-стеке создаёт tools/demo_seed.py (пароль — из SELFVOY_DEMO_PASSWORD) или
-- «Сбросить пароль» с SELFVOY_MAIL_DRIVER=dev.
--
-- psql "$SELFVOY_DATABASE_URL" -f schema/seed.sql

BEGIN;

INSERT INTO users (email, password_hash, display_name, timezone, email_verified)
VALUES
    ('anna@example.com', '!', 'Анна', 'Europe/Moscow', true),
    ('anya@example.com', '!', 'Аня', 'Europe/Moscow', true)
ON CONFLICT DO NOTHING;

INSERT INTO chats (kind, title, created_by)
SELECT 'agent', 'Анна ↔ Аня (Selfvoy)', u.id FROM users u WHERE u.email = 'anna@example.com'
ON CONFLICT DO NOTHING;

INSERT INTO chat_members (chat_id, user_id, role)
SELECT c.id, u.id, 'member'
FROM chats c CROSS JOIN users u
WHERE c.kind = 'agent' AND u.email IN ('anna@example.com', 'anya@example.com')
ON CONFLICT DO NOTHING;

INSERT INTO user_memory (user_id, kind, text, weight, tags)
SELECT u.id, 'preference', v.text, v.weight, v.tags
FROM users u, (VALUES
    ('Люблю тихие кофейни', 2.0, ARRAY['place']),
    ('Не ем мясо', 1.5, ARRAY['diet']),
    ('Работаю над стартапом', 1.0, ARRAY['work'])
) AS v(text, weight, tags)
WHERE u.email = 'anna@example.com'
ON CONFLICT (user_id, text) DO NOTHING;

UPDATE user_preferences p
SET diet = '{vegan}', city = 'Керкраде', lat = 50.861, lon = 6.064, budget_limit = 3.0
FROM users u
WHERE p.user_id = u.id AND u.email = 'anna@example.com';

UPDATE user_preferences p
SET diet = '{gluten_free}', city = 'Херлен', lat = 50.888, lon = 5.978
FROM users u
WHERE p.user_id = u.id AND u.email = 'anya@example.com';

COMMIT;
