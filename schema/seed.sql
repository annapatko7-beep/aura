-- Демонстрационные данные: два пользователя, чат между ними, память и настройки.
-- Пароль у обоих — `aura1234`. Хэши — в старом формате PBKDF2 (созданы до auth v3);
-- они проверяются crypto::verifyPassword и при первом успешном входе прозрачно
-- пересчитываются в Argon2id. Email считается подтверждённым (email_verified
-- по умолчанию TRUE).
--
-- psql "$AURA_DATABASE_URL" -f schema/seed.sql

BEGIN;

INSERT INTO users (email, password_hash, display_name, timezone)
VALUES
    ('anna@example.com',
     'pbkdf2$120000$2bb4723cdd3e4fe8$70b38598e68da2f70c737d16656c9e522896fadce3c1eb64b25db05eb8b544c9',
     'Анна', 'Europe/Moscow'),
    ('anya@example.com',
     'pbkdf2$120000$2bb4723cdd3e4fe8$70b38598e68da2f70c737d16656c9e522896fadce3c1eb64b25db05eb8b544c9',
     'Аня', 'Europe/Moscow')
ON CONFLICT DO NOTHING;

INSERT INTO chats (kind, title, created_by)
SELECT 'agent', 'Анна ↔ Аня (Ауры)', u.id FROM users u WHERE u.email = 'anna@example.com'
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
