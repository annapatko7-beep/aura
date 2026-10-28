-- Общие счётчики попыток для нескольких экземпляров сервера. См. docs/DATABASE.md.
-- Счётчики попыток, общие для всех экземпляров сервера (вход, коды, 2FA, лимиты
-- гостевых методов): окно [window_start, window_start + window_sec). Истёкшие
-- строки сервер периодически удаляет.
CREATE TABLE IF NOT EXISTS rate_limits (
    key              TEXT          PRIMARY KEY,
    count            INTEGER       NOT NULL,
    window_start     TIMESTAMPTZ   NOT NULL,
    window_sec       INTEGER       NOT NULL CHECK (window_sec > 0)
);
