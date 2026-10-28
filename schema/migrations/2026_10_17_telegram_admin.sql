-- F7: ИИ-администратор страницы записи в Telegram. Идемпотентно (можно запускать
-- повторно). См. docs/DATABASE.md.

-- Поле страницы: бот отвечает гостям, показывает свободное время и записывает (Pro).
ALTER TABLE booking_pages ADD COLUMN IF NOT EXISTS ai_admin BOOLEAN NOT NULL DEFAULT false;

-- Чат гостя, привязанный к странице командой /start p_<slug>: один чат — одна страница.
-- Данные — по минимуму: имя и ник из Telegram, последние ≤ 10 сообщений (контекст ИИ).
-- /stop и блокировка бота (403) удаляют строку.
CREATE TABLE IF NOT EXISTS telegram_guest_chats (
    chat_id          BIGINT        PRIMARY KEY,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    guest_name       TEXT          NOT NULL DEFAULT '',
    username         TEXT          NOT NULL DEFAULT '',
    history          JSONB         NOT NULL DEFAULT '[]'::jsonb,
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ   NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS telegram_guest_chats_page_idx ON telegram_guest_chats (page_id);

-- Вопросы гостей, переданные специалисту (escalate): уведомление telegram.question,
-- ответ — telegram.reply.
CREATE TABLE IF NOT EXISTS telegram_questions (
    id               BIGSERIAL     PRIMARY KEY,
    owner_id         BIGINT        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    page_id          BIGINT        NOT NULL REFERENCES booking_pages(id) ON DELETE CASCADE,
    chat_id          BIGINT        NOT NULL,
    guest_name       TEXT          NOT NULL DEFAULT '',
    username         TEXT          NOT NULL DEFAULT '',
    text             TEXT          NOT NULL,
    status           TEXT          NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'answered')),
    answer           TEXT          NOT NULL DEFAULT '',
    created_at       TIMESTAMPTZ   NOT NULL DEFAULT now(),
    answered_at      TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS telegram_questions_owner_idx ON telegram_questions (owner_id, id DESC);
